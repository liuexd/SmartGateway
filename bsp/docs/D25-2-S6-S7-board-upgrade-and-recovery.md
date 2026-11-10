# D25-2 S6/S7：板端 OTA 升级、回滚与掉电恢复

## 一、范围

S1–S5 全部在 x86 上完成。本阶段把同一套流程搬到真实硬件，
用真实断电验证持久性。

| 阶段 | 内容 | 判据 |
|------|------|------|
| S6 | 板端正常升级 + 北向故障回滚 | current 切换正确、业务恢复 |
| S7 | 升级中途掉电 + 重启恢复 | current 不悬空、状态可解释、可收敛 |

**硬件**：100ASK i.MX6ULL Pro，Linux 4.9.88，Cortex-A7，eMMC
**部署路径**：`/opt/smartgateway`（已验证跨重启持久）

## 二、部署结构

```
/opt/smartgateway/
├── bin/
│   ├── ota_agent            245748 B  ARM ELF32 hard-float
│   └── service_supervisor    62248 B
├── current -> releases/v0.x.0
├── releases/
│   └── v0.x.0/
│       ├── smart_gateway
│       └── manifest.json
├── packages/                升级包（放在持久目录，不放 /tmp）
├── run/                     supervisor.sock + service.pid
├── logs/
├── service.conf
└── state/ota_state.json
```

### 一个踩到的坑：`/tmp` 是 tmpfs

第一次把升级包放在 `/tmp/pkg-v0.6.0`，断电重启后包消失，
后续验证全部报 `invalid manifest JSON`。

这不是产品缺陷，但值得记录：**任何需要跨重启存在的东西
都不能放 `/tmp`**。板端 `/tmp` 是 tmpfs（245M），
`/` 是 ext4（714M 可用）。

## 三、S6-1：正常升级 v0.2.0 → v0.3.0

```
Pre-upgrade gateway state: RUNNING
PASS: gateway stopped                 <- supervisor 停止
PASS: candidate activated: 0.3.0
Gateway started: pid=21453            <- supervisor 启动
PASS: L1 TCP ready
PASS: southbound DATA accepted
PASS: L2 probe forwarded, seq=619106750
PASS: L2 business health
PASS: Gateway stopped
PASS: OTA upgrade CONFIRMED: 0.3.0

State     : CONFIRMED
Confirmed : 0.3.0
Current   : releases/v0.3.0
```

关键点：`Pre-upgrade gateway state: RUNNING` 说明 supervisor
正确报告了正在运行的实例，OTA 因此走「先停」而不是「直接激活」。

## 四、S6-2：北向故障回滚 v0.3.0 → v0.4.0

故障注入方式：`OTA_UPSTREAM_PORT=1`（该端口无人监听，
真实返回 `ECONNREFUSED`，不是测试开关短路）。

```
Pre-upgrade gateway state: RUNNING
PASS: gateway stopped
PASS: candidate activated: 0.4.0
PASS: L1 TCP ready
PASS: southbound DATA accepted
FAIL: northbound refused (ECONNREFUSED) port=1
FAIL: no northbound business ack within L2 window
FAIL: L2 business health
PASS: Gateway stopped
RECOVERY: attempting rollback
RECOVERY: state=ACTIVATED, current=releases/v0.4.0
RECOVERY: current restored
PASS: rollback completed
RECOVERY: new mock upstream port: 55977
RECOVERY: verifying old Gateway business
PASS: L1 TCP ready
PASS: L2 business health
PASS: old Gateway business recovered

State     : ROLLED_BACK
Confirmed : 0.3.0
Current   : releases/v0.3.0
```

### 「回滚后 supervisor 报 stopped」是正确行为

第一次看到这个结果时以为出了问题：

```
--- 回滚后状态 ---
State     : ROLLED_BACK
Current   : releases/v0.3.0
--- 业务是否恢复 ---
status: stopped pid=0 starttime=0      <- 看起来业务没起来
```

实际日志里明确有 `PASS: old Gateway business recovered`。
原因是 **OTA 在结束前会停掉它启动的测试实例**：

```
PASS: old Gateway business recovered   <- 验证业务可用
PASS: Gateway stopped                  <- 验证完再停掉
```

这是有意的：升级事务不应留下一个由它启动、却不在 supervisor
正常管理下的进程。升级结束后服务由 init/运维按部署策略启动。

**但这是一个必须明确说明的语义**：`OTA 返回 ROLLED_BACK` ≠
`业务正在运行`。要恢复业务需要显式启动：

```
$ ./bin/service_supervisor start --root /opt/smartgateway
start: OK started
$ netstat -ltn | grep 7000
tcp  0  0  0.0.0.0:7000  0.0.0.0:*  LISTEN
```

## 五、S7：升级中途掉电

### 5.1 如何精确命中掉电窗口

这一步失败了三次才成功，过程本身值得记录。

**失败一**：等 `candidate activated` 后 reboot —— 升级太快，
在 reboot 生效前就跑完了（`PASS: OTA upgrade CONFIRMED: 0.5.0`）。

**失败二**：用静默上游让 L2 卡住 —— Gateway 对静默上游的
重试有超时，`L1 TCP ready` 之后 L2 也很快结束。

**失败三**：包放在 `/tmp`，重启后消失。

**成功做法**：监视 `current` 软链变化，一旦指向新版本立即 reboot：

```sh
BEFORE=$(readlink current)
( OTA_UPSTREAM_PORT=47821 timeout 200 ./bin/ota_agent upgrade-test ... ) &
for i in $(seq 1 900); do
  NOW=$(readlink current 2>/dev/null)
  if [ "$NOW" != "$BEFORE" ]; then break; fi
  sleep 0.02
done
sync
(sleep 0.1; reboot) &
```

`sync` 是必须的：不落盘的话掉电丢掉的可能不是我们想测的东西。

### 5.2 掉电瞬间的状态

```
current 已切换: releases/v0.5.0 -> releases/v0.6.0

last_operation.json:
{
  "format_version": 1,
  "phase": "STARTING_NEW",
  "pid": 686,
  "started_at": 367968,
  "boot_id": "d97ed791-8dcd-49f0-8470-63eb91bf2317"
}

state/ota_state.json:
  "state":"ACTIVATED"
```

这是一个**真实的危险中间态**：

| | 值 | 含义 |
|---|---|---|
| `current` | `releases/v0.6.0` | 磁盘上的「可用版本」已是新版 |
| `state` | `ACTIVATED` | 事务尚未确认 |
| `last_operation` | `STARTING_NEW` | 操作停在「正在启动新版」 |
| 实际进程 | 无 | 旧版已停、新版未起 |

若没有 `last_operation.json`，下次开机看到的会是
「state 是 ACTIVATED，current 指向新版」—— 看起来像一次
正常但未完成确认的升级，而**没有任何线索说明业务其实是断的**。

### 5.3 重启后的验证

```
$ cat /proc/uptime
30.83 9.95                                  <- 确实是重启后

current: releases/v0.6.0
"state":"ACTIVATED"
last_operation.json 仍在，phase=STARTING_NEW

$ test -d "$(readlink current)"
OK: 目录存在                                 <- F9：current 不悬空
```

**F9 的核心判据通过**：`current` 指向的目录真实存在，
没有出现悬空软链。这是原子 `symlink + rename` 的结果 ——
软链接要么指向旧目标，要么指向新目标，不存在中间态。

### 5.4 OTA 拒绝自动继续

```
$ ./bin/ota_agent upgrade-test packages/v0.6.0 /opt/smartgateway
FAIL: previous upgrade interrupted at phase STARTING_NEW
      recorded pid=686, started_at=367968
      record is from a previous boot (system restarted mid-upgrade)

  The gateway may be stopped. Check whether it is
  running, then either restore service manually or
  acknowledge to allow further upgrades:
    ota_agent ack-operation /opt/smartgateway
```

三点都做到了：

1. **指出中断阶段**（`STARTING_NEW`）
2. **识别为上一次开机**（`boot_id` 不匹配 → 必然是掉电/硬重启）
3. **给出解除路径**，而不是让人去手工 `rm`

第 2 点尤其重要：同一个 `phase=STARTING_NEW` 可能来自
「OTA 进程被 kill -9」（本次开机）或「整机掉电」（上次开机）。
两者的排查方向完全不同。

### 5.5 recover 收敛

```
$ ./bin/ota_agent recover /opt/smartgateway
RECOVERY: state=ACTIVATED, current=releases/v0.6.0
RECOVERY: current restored
PASS: rollback completed

$ ./bin/ota_agent status /opt/smartgateway
State     : ROLLED_BACK
Confirmed : 0.5.0
Current   : releases/v0.5.0
Release   : CONFIRMED_TARGET
```

`recover` 不依赖 `last_operation.json` 做决策 —— 它只看
`state` + `current` 的一致性（这是权威信息）。`last_operation`
只用于「告诉人上次停在哪」。

### 5.6 业务恢复

```
$ ./bin/service_supervisor start --root /opt/smartgateway
start: ALREADY already running (pid=521)
status: running pid=521 starttime=3574
tcp  0  0  0.0.0.0:7000  0.0.0.0:*  LISTEN
```

注意 `ALREADY`：supervisor 由 init 在开机时已拉起并恢复了 Gateway。

### 5.7 完整闭环

```
$ ./bin/ota_agent ack-operation /opt/smartgateway
Acknowledging interrupted operation:
  phase      : STARTING_NEW
  pid        : 686
  started_at : 367968
PASS: operation record cleared; upgrades are allowed

$ ./bin/ota_agent upgrade-test packages/v0.6.0 /opt/smartgateway
Pre-upgrade gateway state: RUNNING
PASS: OTA upgrade CONFIRMED: 0.6.0
```

从「掉电残留」到「系统完全正常并可以继续升级」，
全链路板端实测通过。

## 六、S6/S7 覆盖到的失败模式

| 编号 | 场景 | 板端结果 |
|------|------|----------|
| F2 | 升级中途掉电 | 检测到 `STARTING_NEW`、识别跨重启、拒绝继续、可 ack |
| F4/F5 | L1/L2 失败 | 回滚到旧版，`current` 正确 |
| F9 | 掉电后 `current` 完整性 | 无悬空软链，目录存在 |
| — | 回滚后业务可用性 | `old Gateway business recovered` 实测通过 |
| — | 重启后 supervisor 持久性 | init 自动拉起，Gateway 状态正确 |

## 七、未覆盖（必须明确）

| 编号 | 内容 | 原因 |
|------|------|------|
| L10 | SIGKILL 也无效（D 状态） | 板端无法构造 D 状态（无 FUSE 开发包、无 NFS） |
| L12 | supervisor 崩溃自愈（G4b） | 需 `/etc/inittab` 的 `respawn`，与 `start-stop-daemon` 的 pidfile 管理冲突，未实现 |
| — | 串口模式升级 | 本次全部以 `serial=`（空）即 `--no-serial` 运行；串口路径未在板端验证 |
| — | 真实北向服务器 | 上游为本地 Mock，未接真实云平台 |
| — | eMMC 坏块/只读挂载 | 未做存储层故障注入 |

**关于「串口模式未验证」**：`service.conf` 里 `serial=` 留空时
supervisor 传 `--no-serial`。板端真实部署应配置
`serial=/dev/ttymxc2`。这条路径的代码分支（参数拼装）
在 x86 单测里覆盖过，但**没有在板端跑过真实串口**，
因此不报告为 PASS。

## 八、结论

- S6：板端正常升级（v0.2.0→v0.3.0）与北向故障回滚
  （v0.3.0→v0.4.0）全部通过，`current`/`state`/业务三者一致。
- S7：真实断电命中 `STARTING_NEW` 窗口，重启后 `current` 不悬空、
  OTA 拒绝自动继续、`recover` 收敛到 `ROLLED_BACK`、
  `ack-operation` 后系统可继续升级 —— 完整闭环。
- 掉电场景的价值不在于「恢复成功」，而在于**残留状态是可解释的**：
  知道停在哪一步、知道是掉电还是被 kill、知道该恢复还是该确认。
- 语义澄清：`OTA 返回 ROLLED_BACK` **不等于**业务正在运行。
  回滚验证完业务可用性后会停掉测试实例，恢复业务需显式启动。
