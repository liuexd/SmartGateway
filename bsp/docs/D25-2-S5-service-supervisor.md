# D25-2 S5：Service Supervisor 与 BusyBox init 集成

## 一、S5 要解决什么

S4 结束时还剩两个**无法在 OTA 侧消除**的缺陷：

| 编号 | 缺陷 | 为什么 OTA 自己解决不了 |
|------|------|------------------------|
| L3 | 前置校验依赖 pidfile 存在 | 若 Gateway 由别的方式启动（手工执行、旧 init 脚本），pidfile 不存在，OTA 判定 STOPPED 并启动第二个实例。**OTA 无法知道一个它没启动的进程是否存在。** |
| L6 | 停止外部进程的 TOCTOU 窗口 | OTA 读到 `(pid, starttime)` 之后、发信号之前，进程可能退出且 pid 被复用。pidfd 需要 Linux 5.1+，板端是 **4.9.88**，窗口无法消除。 |

两者的根因是同一件事：**OTA 不拥有那个进程**。它只能靠 pidfile 猜测，靠 pid 去发信号。

### 解决办法：把所有权交给常驻 supervisor

```
之前：  OTA ──fork──> Gateway        （OTA 是父进程，但只在升级期间存在）
        OTA ──kill──> 未知进程        （TOCTOU 窗口）

之后：  init ──> Supervisor ──fork──> Gateway
                  ↑
                  └── OTA 只发 IPC 请求，从不直接碰进程
```

Supervisor 是 Gateway 的**父进程**，因此：

- `waitpid` 总是有效 —— 能确认「我启动的那个」确实退出了
- pidfile 由它维护 —— 不可能与真实状态不一致
- 停止作用于自己的子进程 —— **pid 不可能被复用**（未被回收的子进程 pid 不会被内核重新分配）

L6 因此不是「缩小窗口」，而是**结构化地消失**：不是靠更仔细的检查，而是靠所有权。

## 二、职责边界（守住这一条）

```
Supervisor：只管进程。启动、停止、查状态。
OTA       ：只管版本。状态机、releases 目录、current 切换。
```

Supervisor **不读** OTA 状态、**不判断**版本、**不碰** releases 目录、**不参与**「该不该升级」。

一旦让它参与 OTA 决策，就会出现第二个真相源，而两个真相源迟早会不一致。

## 三、为什么 supervisor 不自动重启 Gateway

`G4a`（开机启动）与 `G4b`（崩溃自愈）是两件不同的事。S5 明确只做前者，后者交给 init 的 `respawn`。

**supervisor 不自动拉起 Gateway 的理由**：

OTA 升级期间需要 Gateway 保持停止（旧版退出 → 切换 current → 新版上场）。若 supervisor 自动重启，会在升级中途把**旧版**拉起来，与「切换 current」形成竞争，最终可能出现「进程跑旧版、磁盘上是新版」。

板端实测确认了这个设计：

```
$ /etc/init.d/S99smartgateway start
supervisor: running
gateway: status: stopped pid=0 starttime=0      <- 没有自动启动
$ ls run/service.pid
ls: cannot access 'run/service.pid': No such file
```

## 四、协议设计

Unix Domain Socket + 单行文本，`<install>/run/supervisor.sock`，权限 `0600`。

### 为什么不 JSON / 不 TCP

| 选择 | 理由 |
|------|------|
| 不用 JSON | 请求只有两三个字段。文本单行已足够，且出错时可用 `nc` 手工调试（板端没有 json 工具）。解析代码短，攻击面小。 |
| 不用 TCP | 不需要跨机器。Unix socket 有文件权限保护。 |
| 用真实路径而非抽象命名空间 | 抽象命名空间无法用文件权限限制，且 `ss` 看不到它 —— 「为什么连不上」会变成猜谜。真实路径可以 `ls -l` 看权限、看文件在不在。 |

### 请求集（刻意极小）

```
STATUS
START [<port> <server>]     <- 可选覆盖北向地址
STOP
RELOAD
```

`START` 的参数覆盖是为了让 OTA 的健康检查能指向 Mock Server。覆盖**只对本次启动有效，不写回配置文件** —— 否则一次健康检查会把生产配置永久改成测试地址。

### 错误必须可区分

```
ENOENT        socket 不存在      -> supervisor 没装/没启动
ECONNREFUSED  文件在但无人监听    -> supervisor 崩了
ETIMEDOUT     supervisor 忙      -> 可能在等进程停止
EINVAL        响应无法解析        -> 协议版本不一致
```

笼统地报「连接失败」会让运维无法判断该做什么。

## 五、实现要点

### 5.1 僵尸必须判为不存活

```c
if (state == 'Z' || state == 'X' || state == 'x')
    return 0;      /* 不算存活 */
```

只判断 `/proc/<pid>` 是否存在是不够的：僵尸的目录会一直留到被回收。若把僵尸当成 RUNNING，supervisor 会拒绝重启一个**其实早已死掉**的服务。

### 5.2 陈旧 socket 的判定

```c
if (lstat(path) == 已有 socket) {
    probe = connect(...);
    if (probe >= 0)  -> EADDRINUSE（另一个 supervisor 在跑）
    else             -> 陈旧文件，unlink 后重建
}
```

**不能直接 `unlink` 再 `bind`**：那会让两个 supervisor 同时认为自己是唯一的，于是各启动一个 Gateway。

### 5.3 两个 pidfile，不能混用

| 文件 | 所有者 | 记录什么 |
|------|--------|----------|
| `<root>/run/supervisor-init.pid` | init 的 `start-stop-daemon` | supervisor 自己（用于 `rcS` 层的启停判断） |
| `<root>/run/service.pid` | supervisor | Gateway（用于状态查询） |

混用会导致 init 认为「supervisor 在跑」时其实只是 Gateway 在跑。

## 六、OTA 侧的改动：强制 IPC，不做回退

这是一个 deliberate 的选择。

### 拒绝「保留 pidfile 回退路径」

看似更安全的「两条路」实际更危险：回退路径会在 supervisor 挂掉时**悄悄**启用，而它正是那个有 L3/L6 缺陷的实现。运维看到「升级成功」，却不知道走的是已知有缺陷的路径。

### 代价与方向

强制 IPC 的代价是「supervisor 挂了就不能升级」。

**这是正确的失败方向**：拒绝升级不破坏任何东西；走缺陷路径可能杀掉无关进程。

实测确认：

```
$ ./ota_agent upgrade-test pkg install     # 无 supervisor
FAIL: cannot query gateway state from the supervisor
      the supervisor must be running before an upgrade
      start it with: /etc/init.d/S99smartgateway start
```

### 删除 `port_owned_by_pid` 的错误（本阶段最值得记的一次）

S5 重写 `restore_gateway` 时，我把 S3-R4 加的 `port_owned_by_pid()` 删掉了，理由是「supervisor 是父进程，它的状态必然正确」。

**这个推理漏掉了时间维度**：父进程确认子进程退出需要一次轮询（200ms），在那之前它的视图与事实不一致。

实测症状：

```
FAIL: TCP 7000 still occupied after stopping gateway
RECOVERY: gateway restarted, pid=144190
PASS: old gateway restored and L1-ready      <- 假的
```

实际情况：外部进程占着 7000，supervisor 启动的 Gateway 因 bind 失败立即退出，而 `connect` 打到了外部进程上。

同一个假阳性在 S3-R4 出现过一次，当时用 `port_owned_by_pid` 修掉，S5 又把它带回来。

**教训**：「可观察性不等于正确性」。「能连上」是一个观察，「由我启动的进程在服务」才是事实。修法是把两方面的证据结合：

```
supervisor 说「我的子进程 pid 是 P」
      +  /proc/net/tcp 说「7000 的监听者是 P」
```

两条同时成立才宣称恢复成功。

## 七、验证

### 7.1 单元与协议测试

| 项 | 结果 |
|---|---|
| `service_ipc_test` | **53/53 PASS** |
| `service_supervisor_test` | **35/35 PASS** |
| `test_s5_supervisor_e2e` | **33/33 PASS** |

`service_supervisor_test` 的重点是**僵尸**与**pid 复用**：

```
PASS: 前置：进程已进入僵尸态
PASS: 僵尸判为未运行（关键）
PASS: starttime 不匹配判为未运行
PASS: 进程本身未被影响           <- 试金石
```

`test_s5_supervisor_e2e` 覆盖真实进程生命周期：

```
用例一：supervisor 未运行时错误提示可区分
用例二：完整生命周期（start/幂等/stop/pidfile 一致）
用例三：Gateway 崩溃后状态一致性
用例四：忽略 SIGTERM -> SIGKILL
用例五：supervisor 崩溃后的陈旧 socket 自动恢复
用例六：重复 supervisor 被拒绝
```

### 7.2 板端实测（i.MX6ULL，4.9.88）

部署：`/opt/smartgateway/{bin,releases,run,logs}` + `/etc/init.d/S99smartgateway`。

```
$ ./bin/service_supervisor start --root /opt/smartgateway
start: OK started
$ ./bin/service_supervisor status --root /opt/smartgateway
status: running pid=2155 starttime=36506128
$ cat run/service.pid
2155 36506128
$ readlink /proc/2155/exe
/opt/smartgateway/releases/v0.1.0/smart_gateway
$ netstat -ltn | grep 7000
tcp  0  0 0.0.0.0:7000  0.0.0.0:*  LISTEN
```

崩溃检测（`kill -9` Gateway）：

```
status: stopped pid=0 starttime=0
pidfile 已清理
```

真实重启（`reboot`，uptime 归零后重新 SSH）：

```
$ cat /proc/uptime
41.07 ...                                    <- 确实是重启后
$ /etc/init.d/S99smartgateway status
supervisor: running                          <- init 自动拉起
gateway: status: stopped pid=0 starttime=0   <- 没有擅自启动 Gateway（正确）
```

陈旧 socket（`kill -9` supervisor，文件残留 `srw-------`）：

```
$ /etc/init.d/S99smartgateway start
smartgateway started                         <- 自动清理并重建
```

### 7.3 板端完整 OTA 升级（S5 的最终验收）

```
Pre-upgrade gateway state: RUNNING
PASS: gateway stopped              <- supervisor 停止
PASS: candidate activated: 0.2.0
Gateway started: pid=21453         <- supervisor 启动
PASS: L1 TCP ready
PASS: southbound DATA accepted
PASS: L2 probe forwarded, seq=844380910
PASS: L2 business health
PASS: Gateway stopped
PASS: OTA upgrade CONFIRMED: 0.2.0

State     : CONFIRMED
Confirmed : 0.2.0
Current   : releases/v0.2.0
```

### 7.4 L3 解决的直接证据

这是 S3/S4 完全做不到的：

```
1) supervisor 状态:  status: running pid=21506
2) pidfile:          21506 196779
3) 实际进程:         21506 -> .../v0.2.0/smart_gateway
4) 删掉 pidfile 后:  status: running pid=21506     <- 仍然正确！
5) 再次 stop:        stop: OK stopped              <- 仍然能停！
```

**pidfile 被删掉，supervisor 依然知道真实状态并能停止进程** —— 因为所有权在它的内存里（子进程关系），不在文件里。

这正是 L3 的根因：旧实现把「真实状态」寄托在一个可能不存在、可能被篡改、可能过期的文件上。

## 八、回归

S5 改动 OTA 的核心 Gateway 管理路径，全部既有测试都需要适配。

| 项 | S5 前 | S5 后 | 说明 |
|---|---|---|---|
| D23 北向故障 | 27/27 | **27/27** | 升级流程注入 supervisor 环境 |
| S3 前置校验 | 75/75 | **75/75** | 场景一/二/三按新架构重写 |
| S3-R4 恢复路径 | 38/38 | **38/38** | 场景 B 加 supervisor |
| S4 故障矩阵 F1–F10 | 66/67 | **67/67** | F1/F7/F8 按新语义重写 |
| `test_s5_supervisor_e2e` | — | **33/33** | 新增 |
| `service_ipc_test` | — | **53/53** | 新增 |
| `service_supervisor_test` | — | **35/35** | 新增 |
| x86 单元测试 | 全 PASS | **全 PASS** | |
| ARM 交叉编译 | — | **PASS** | ota_agent 245748B, supervisor 62248B |

### 测试适配中发现的三个真实缺陷

**其一：`ota_health` 的 `child_alive` 用 `waitid`**

```
waitid(P_PID, pid, WEXITED|WNOHANG|WNOWAIT)
```

它只在**本进程是父进程**时有效。S5 之后 Gateway 由 supervisor fork，OTA 不是父进程，`waitid` 返回 `ECHILD`，于是 L1 检查立刻判「进程已死」。

实测症状：Gateway 确实在跑并监听 7000，但输出 `FAIL: L1 health` 并触发回滚。

这个缺陷隐蔽之处在于：**错的是前提，不是逻辑**。返回值被正确检查了，只是失败原因变了。改用 `/proc/<pid>/stat` 的 state 字段判断。

**其二：测试遗留 supervisor 进程**

场景中途失败时 `SupervisorEnv.stop()` 未被执行，泄漏的 supervisor 一直占着 7000，后续所有场景都因「端口已被占用」无法开始 —— 而症状看起来像被测代码的问题。

修法：`stop()` 增加两道兜底（按 `exe_prefix` 清本目录 Gateway，按 `--root` 参数精确匹配清 supervisor）。

**其三：`cleanup()` 把 `SupervisorEnv` 当成 `Popen`**

只调 `terminate()`，而 `SupervisorEnv` 没有这个方法（异常被 `except` 吞掉），于是 Gateway 和 supervisor 都没有被清理。

修法：先探测 `stop` 属性。

## 九、遗留限制

| 编号 | 内容 | 状态 |
|---|---|---|
| L1 | 停止超时后不无限重试 | 由 supervisor 承担：SIGTERM → SIGKILL → 失败即报告 |
| L2 | legacy pidfile 迁移 | OTA 侧已不再依赖，问题消失 |
| L3 | 前置校验依赖 pidfile | **已解决**（板端实测：删掉 pidfile 仍正确） |
| L4 | 「停不掉」场景未板端验证 | D 状态无法在本机构造，见 L10 |
| L5 | 无 CI | 所有 PASS 仍为手工执行 |
| L6 | 停止外部进程 TOCTOU | **已解决**（supervisor 停自己的子进程） |
| L7 | starttime 精度不足 | 不再作为安全判据的唯一来源 |
| L8 | `port_owned_by_pid` 不覆盖 IPv6 | 当前绑 `0.0.0.0`，不影响 |
| L10 | `survived SIGKILL` 分支无集成测试 | 本机无法构造 D 状态（无 FUSE 头、无 NFS、无 root） |
| **L12** | **respawn 未注册到 inittab** | 当前 supervisor 由 `S99smartgateway` 拉起一次；supervisor 崩溃后需人工或重启。完整的 G4b 需要 `/etc/inittab` 加 `::respawn:`，**本阶段未做**，且会与 `start-stop-daemon` 的 pidfile 管理冲突，需单独设计 |

### L12 的说明（必须诚实记录）

init 脚本只保证 **supervisor 被启动一次**。若 supervisor 本身崩溃，`rcS` 不会重新执行，服务就此消失直到下次重启。

真正的 G4b（supervisor 崩溃自愈）需要把 supervisor 注册到 BusyBox init 的 `respawn`：

```
::respawn:/opt/smartgateway/bin/service_supervisor run --root /opt/smartgateway
```

但这样 `start-stop-daemon --make-pidfile` 就失去意义（进程不再由它 fork），需要重做启停逻辑。**本阶段没有做这个改动**，因为：

1. 它会与现有 `S99smartgateway` 的 pidfile 管理直接冲突
2. 需要先决定「init 管进程」还是「init 管脚本」，这是设计决策不是实现细节
3. 当前设计的核心价值（L3/L6）不依赖它

因此 G4b 列为**未实现**，不得报告为 PASS。

## 十、结论

- L3 与 L6 通过**转移所有权**解决，而非更好的检查。板端实测确认：删掉 pidfile 后 supervisor 依然正确。
- 强制 IPC、不做回退：supervisor 不可用即拒绝升级。代价明确，方向正确。
- 板端完整链路（init 启动、真实 OTA 升级、崩溃检测、陈旧 socket、真实重启）全部实测通过。
- 过程中修复 3 个真实缺陷，其中「删掉 `port_owned_by_pid` 导致假阳性重现」这一条最有价值 —— 它说明**同一个错误会以不同理由回来**。
- G4b（supervisor 崩溃自愈）**未实现**，列为 L12，留给后续阶段。
