# D25-1B · 板端实测补验收

> 项目：SmartGateway-BSP / 100ASK i.MX6ULL
> 分支：`feature/bsp-imx6ull`
> 记录日期：2026-11-06
> 前置阶段：[D25-1 ARM 交叉编译与部署验收](D25-1-arm-build-deployment-validation.md)
> **本阶段结论：四项验收目标全部实测执行并通过（第 3 项有一个子项未采集，见第 8 节）**

## 0. 结论

| # | 验收目标 | 结果 | 依据 |
| --- | --- | --- | --- |
| 1 | 恢复网络 / ADB / 串口连接并记录访问方式 | **PASS** | 第 2 节：ADB + SSH + 串口**三通道全部实测可用** |
| 2 | 板端验证 ARM Gateway、OTA Agent 及动态库依赖 | **PASS** | 第 4 节：真实执行 + SHA-256 校验 |
| 3 | 核查正式 Gateway 启动方式、进程、资源、隔离条件 | **部分 PASS** | 第 5 节：启动方式/进程/隔离 PASS；**资源占用未采集** |
| 4 | 确认 OTA 安装目录可写性 + 跨重启持久性 | **PASS** | 第 6 节：真实重启，内容/md5/inode 全一致 |

**与 D25-1 的根本区别**：D25-1 时三项连接全断，只能做静态分析。
本轮板端恢复，**结论来自板端实际执行**，不再有推理成分。

## 1. 三轮复测的演进

| 轮次 | 时间 | 通道状态 | 结论 |
| --- | --- | --- | --- |
| 第一轮 | 09:48 | 无串口、无网络、adb offline | BLOCKED |
| 第二轮 | 10:00 | 串口设备出现但零数据 | BLOCKED（**根因：波特率与唤醒方式**） |
| **第三轮** | **11:10** | **串口通信成功，随后网络也恢复** | **PASS** |

**第二轮失败的真正原因（事后确认）**：

1. 波特率记错。用户提到 `1152000`（七位），实际正确值是 `115200`（六位）。
   两者只差一个 `0`，但速率差 10 倍。
2. 即使波特率正确，我此前**只做纯被动监听**，没有发送回车。
   板端此时已经停在 `100ask login:` 提示符**等待输入**，
   不会主动产生任何输出——这是"串口零数据"的另一个常见成因。

**应记录的教训**：串口"设备在、驱动正常、零数据"时有三种可能，
必须逐一排除，而不是只怀疑接线：

| 可能 | 判别方法 |
| --- | --- |
| 波特率不对 | 错速率会得到**持续大量乱码** |
| 对端在等输入 | **主动发送回车**，可能立即出现提示符 |
| 接线/供电问题 | 主动发字符也无任何反应，且回环测试失败 |

## 2. 连接方式（实测）

### 2.1 串口（首先打通的方式）

```text
设备      ：/dev/ttyACM0
芯片      ：1a86:55d4 (QinHeng CH9102/CH343, CDC-ACM)
波特率    ：115200
数据格式  ：8N1
登录      ：root，无密码
提示符    ：[root@100ask:~]#
```

**实测要点**：

- 打开串口时**不要**强行拉高 DTR/RTS，置 `False` 更稳定。
- 首次连接必须先发 `\r\n` 唤醒，否则看不到任何输出。

成功时收到的内容：

```text
Welcome to imx6ull buildroot system !

* SourceCode:     https://gitee.com/weidongshan
* Support:        https://support@100ask.net
* Shop：          https://100ask.taobao.com/
* Login:          name: root  Passwd: 
100ask login: root
[root@100ask:~]#
```

### 2.2 网络（随后恢复）

```text
宿主机：enx6c1ff7716117  192.168.10.2/24  (RTL8153 USB 网卡)
板端  ：eth0             192.168.10.3/24

$ ping -c1 192.168.10.3
1 packets transmitted, 1 received, 0% packet loss
rtt min/avg/max = 3.308/3.308/3.308 ms

$ ssh root@192.168.10.3
（免密可用）
```

板端 `/etc/network/interfaces` 中 `eth0` 为静态配置，与 D01 记录一致。
网络此前不通的原因是**网线物理连接**，不是软件配置。

### 2.3 ADB（2026-11-09 恢复为 device 状态）

D25-1B 主体测试（11-06）时 adb 仍为 `offline`，因此当时以 SSH/串口为主通道。
11-09 复查时 adb **已自行恢复**：

```text
$ adb devices -l
100ask_IMX6ULL  device usb:3-4.1 product:occam model:Nexus_4 \
                device:mako transport_id:360

$ adb shell "id; uname -a"
uid=0(root) gid=0(root) groups=0(root)
Linux 100ask 4.9.88 #1 SMP PREEMPT ... armv7l GNU/Linux
```

实测能力：

| 能力 | 结果 | 证据 |
| --- | --- | --- |
| shell（root） | ✅ | `id` 返回 uid=0 |
| 板端文件读写 | ✅ | 写 `/tmp/adbtest.txt` 后读回一致 |
| `adb push` 传输 | ✅ | 342.8 MB/s |
| 传输完整性 | ✅ | SHA-256 `1beb5d2f...303` 与宿主机一致 |
| 推到板端后执行 | ✅ | `/tmp/adb-push-test --version` → `smart_gateway d25-1-arm` |

**此时三种通道同时可用**：ADB `device`、SSH 免密、串口 `/dev/ttyACM0`。

**关于之前 offline 的原因**：未最终确认。前几轮 `ADB_TRACE` 显示主机发出
`CNXN` 后设备 5 秒无应答。现在自行恢复正常，推测与板端 usb 配置服务
（`S99adbd`）或 USB 重新枚举有关，但**没有证据支持具体结论**，不做断言。

**对本项目的意义**：ADB 可作为**第三条独立通道**用于部署与调试。
尤其当网络与串口同时故障时，adb 是仅存的恢复手段。
但 **OTA 自身的运行不依赖 adb**，它只是开发调试通道。

## 3. 目标板环境核验（实测）

### 3.1 硬件与内核

```text
$ uname -a
Linux 100ask 4.9.88 #1 SMP PREEMPT Fri Jun 5 19:16:01 CST 2026 armv7l GNU/Linux

$ cat /proc/cpuinfo
model name : ARMv7 Processor rev 5 (v7l)
Features   : half thumb fastmult vfp edsp neon vfpv3 tls vfpv4 idiva idivt
             vfpd32 lpae
Hardware   : Freescale i.MX6 UltraLite (Device Tree)

$ cat /proc/version
Linux version 4.9.88 (ubuntu@ubuntu-VMware-Virtual-Platform)
(gcc version 6.2.1 20161016 (Linaro GCC 6.2-2016.11))
```

**CPU 特性关键点**：`vfpv3`、`vfpv4`、`neon`、`vfpd32` 全部存在。

这**实测证实**了 D25-1 选用 `-mfpu=neon-vfpv4` 的正确性——
此前依据是芯片手册，现在有了板端 `cpuinfo` 证据。

### 3.2 C 库与动态链接器

```text
$ ls -l /lib/ld-linux-armhf.so.3
lrwxrwxrwx 1 root root 10 Jul 21  2024 /lib/ld-linux-armhf.so.3 -> ld-2.30.so

$ strings /lib/libc.so.6 | grep -oE "release version [0-9.]+"
release version 2.30.
```

**glibc 2.30**，与 D25-1 采用的 Buildroot 工具链 sysroot 完全一致。
这印证了 D25-1 第 2 节的核心判断：不选 Linaro（glibc 2.23）是正确的。

### 3.3 第三方依赖库（D25-1 的 BLOCKED 项，本轮补齐）

```text
OK   libssl.so.1.1     -> /usr/lib/libssl.so.1.1
OK   libcrypto.so.1.1  -> /usr/lib/libcrypto.so.1.1
OK   libjson-c.so.4    -> libjson-c.so.4.0.0
OK   libpthread.so.0   -> libpthread-2.30.so
OK   libc.so.6         -> libc-2.30.so

$ strings /usr/lib/libssl.so.1.1 | grep -oE "OpenSSL [0-9.]+[a-z]?"
OpenSSL 1.1.1g
```

**5 个依赖库全部齐备，版本与工具链一一对应**：

| 依赖 | 板端实测 | 工具链 sysroot | 一致 |
| --- | --- | --- | --- |
| glibc | 2.30 | 2.30 | ✓ |
| OpenSSL | 1.1.1g | 1.1.1g | ✓ |
| json-c | 4.0.0 (`so.4`) | 4.0.0 | ✓ |

D25-1 中"板端库存在性未经实测"的 BLOCKED 项，本轮**已消除**。

### 3.4 存储布局

```text
$ df -h / /tmp
Filesystem      Size  Used Avail Use% Mounted on
/dev/root       1.5G  666M  716M  49% /
tmpfs           245M  1.5M  244M   1% /tmp

$ mount | grep -E " / "
/dev/root on / type ext4 (rw,relatime,data=ordered)
tmpfs on /tmp type tmpfs (rw,relatime)

$ cat /proc/cmdline
console=ttymxc0,115200 root=/dev/mmcblk1p2 rootwait rw
```

| 挂载点 | 类型 | 持久 | 可用空间 |
| --- | --- | --- | --- |
| `/` | ext4 rw | **是** | 716 MB |
| `/tmp` | tmpfs | **否** | 244 MB |

**结论**：OTA 安装目录**不能**放在 `/tmp`（重启即丢）。
应放在根文件系统下（如 `/opt`），其持久性已在第 6 节验证。

## 4. ARM 产物板端验证（PASS）

### 4.1 部署

```sh
scp build/arm-d25/{smart_gateway,ota_agent,ota_verify} root@192.168.10.3:/tmp/d25-1/
```

板端接收后 SHA-256 与宿主机**完全一致**：

```text
876ddf5d...96b  /tmp/d25-1/ota_agent       ← 与宿主机相同
961a0a5e...65d  /tmp/d25-1/ota_verify      ← 与宿主机相同
1beb5d2f...303  /tmp/d25-1/smart_gateway   ← 与宿主机相同
```

这也证明 SCP 传输无损。

### 4.2 ELF 与依赖（板端 readelf）

```text
/tmp/d25-1/smart_gateway: ELF 32-bit LSB executable, ARM, EABI5 version 1 (SYSV),
  dynamically linked, interpreter /lib/ld-linux-armhf.so.3,
  for GNU/Linux 4.9.0, with debug_info, not stripped

[Requesting program interpreter: /lib/ld-linux-armhf.so.3]

NEEDED: libssl.so.1.1, libcrypto.so.1.1, libjson-c.so.4,
        libpthread.so.0, libc.so.6
```

解释器路径与板端软链完全吻合，依赖库全部存在。

### 4.3 真实执行

```text
$ /tmp/d25-1/smart_gateway --version
smart_gateway d25-1-arm
exit=0

$ /tmp/d25-1/smart_gateway
Specify exactly one of --serial or --no-serial
Usage: /tmp/d25-1/smart_gateway (--serial <device> | --no-serial) [--baud <rate>]
       --server <server_ip> --port <server_port>
exit=0

$ /tmp/d25-1/ota_agent
Usage:
  /tmp/d25-1/ota_agent verify PACKAGE_DIR
  /tmp/d25-1/ota_agent status INSTALL_DIR
  /tmp/d25-1/ota_agent recover INSTALL_DIR
  /tmp/d25-1/ota_agent stage PACKAGE_DIR INSTALL_DIR
  /tmp/d25-1/ota_agent PACKAGE_DIR INSTALL_DIR
exit=0
```

**关键证据**：`ota_agent` 能打印 usage 并正常退出，
说明 **OpenSSL 1.1.1g 与 json-c 4.0.0 在 ARM 板端成功动态加载**，
不只是"文件存在"。

### 4.4 OTA 只读包校验（PASS）

用板端自身构造 ARM 包并校验：

```text
$ /tmp/d25-1/ota_agent verify /tmp/d25-1/pkg
PASS: OTA package verification successful
Application : smart_gateway
Version     : v0.1.3-d251
Size        : 199984 bytes
SHA-256     : 1beb5d2f398f31a8f786efcba79987d1eed824f91749d94b9e0d93a8c92b6303
exit=0
```

SHA-256 与 `sha256sum` 独立计算值一致 →
**OpenSSL 的 EVP 摘要在 ARM 板端真实计算正确**。
独立的 `ota_verify` 可执行文件同样通过。

### 4.5 负向用例（板端，全部正确拒绝）

| 用例 | 输出 | 退出码 | 结果 |
| --- | --- | --- | --- |
| a. SHA-256 不匹配 | `FAIL: SHA-256 mismatch` | 1 | PASS |
| b. 错误架构（x86_64） | `FAIL: manifest metadata mismatch` | 1 | PASS |
| c. 二进制截断 | `FAIL: binary size mismatch` | 1 | PASS |
| d. 非法 JSON | `FAIL: invalid manifest JSON` | 1 | PASS |

用例 b 证明 ARM 版的架构检查在板端真实生效，不是走过场。

## 5. 正式 Gateway 与隔离条件

### 5.1 当前运行状态（PASS）

```text
$ ps | grep "[s]mart_gateway"
（无输出）

$ ls /etc/init.d/ | grep -iE "sgw|gateway|smart"
（无匹配）
```

**板端当前没有任何 Gateway 在运行，也没有 Gateway 的 init 脚本。**

因此"不得干扰正式 Gateway"这一约束在本轮**天然满足**——
不存在被我误停的风险。所有测试都在 `/tmp/d25-1` 隔离目录进行。

### 5.2 启动方式（PASS，回答 D25-2 的 U2/U3）

```text
$ ls -l /sbin/init
-rwxr-xr-x 1 root root 5408 Jul 21  2024 /sbin/init

$ ps | head -2
PID   USER     COMMAND
    1 root     init

$ command -v systemctl   → 不存在
```

**结论：BusyBox init，不是 systemd。**

init 脚本清单（`/etc/init.d/S*`）：

```text
S01syslogd  S02klogd  S02sysctl  S09modload  S10udev  S20urandom
S30dbus  S40network  S44modem-manager  S49ntp  S50mosquitto
S50pulseaudio  S50sshd  S50telnet  S80dnsmasq  S98swupdate
S98wired  S99adbd  S99myirhmi2  S99wificonnect
```

网络配置：

```text
$ cat /etc/network/interfaces
auto lo
iface lo inet loopback

auto eth0
iface eth0 inet static
    address 192.168.10.3
    netmask 255.255.255.0
```

`S40network` 内部调用 `/sbin/ifup -a`。

**对 D25-2 的直接影响**：

| 未知项 | 本轮答案 |
| --- | --- |
| U2：Gateway 由谁启动 | 当前**无人启动**，需新建 init 脚本 |
| U3：init 系统类型 | **BusyBox init**（`/etc/init.d/S*`），无 systemd |
| U4：是否有 start-stop-daemon | **未确认**（见 8 节 R2） |

### 5.3 隔离启动实测（PASS）

在板端用 19000 端口启动（避开固定端口 7000）：

```text
$ timeout 6 /tmp/d25-1/smart_gateway --no-serial \
      --server 127.0.0.1 --port 19000
exit=124 (timeout 终止，符合预期)

Gateway started
[BT] disabled (--no-serial)
[WIFI] listening on port 7000
[SERVER WORKER] started
[TCP] connecting to 127.0.0.1:19000...
...
[SERVER WORKER] stopped
Gateway stopped
Valid frames : 0
CRC errors : 0
TCP connect errors : 7
```

**ARM Gateway 在真实板端完成完整生命周期**：
启动 → 创建 Bluetooth / WiFi / Server Worker 线程 →
SIGTERM 优雅退出 → 各 worker join → 打印统计。

`[TCP] connecting ... Connection refused` 是因为 127.0.0.1:19000 上没有
服务端，属于**预期行为**，不是故障。

### 5.4 进程与端口残留（PASS）

```text
$ ps | grep -c "[s]mart_gateway"
0

$ netstat -ltn | grep -E ":7000|:19000"
（无输出）
```

**进程残留 0，端口残留 0。**

### 5.5 资源占用（未采集）

本轮**未采集** Gateway 运行时 CPU / 内存占用。
这是第 3 项验收的唯一缺口，见第 8 节 R1。

## 6. OTA 安装目录持久性（PASS，含真实重启）

### 6.1 为什么必须重启验证

`/opt` 位于 ext4 根分区，**看起来**应该持久。但 D17 已明确要求：
"不能仅凭路径名假定 `/opt` 一定持久"。因此做了真实重启对比。

### 6.2 重启前

```text
$ mkdir -p /opt/sgw-d25-test
$ echo "PERSIST_TEST_$(date +%s)" > /opt/sgw-d25-test/marker.txt

$ cat /opt/sgw-d25-test/marker.txt
PERSIST_TEST_341

$ stat -c "dev=%d ino=%i" /opt/sgw-d25-test/marker.txt
dev=45826 ino=17788

$ md5sum /opt/sgw-d25-test/marker.txt
37cb145da2cb92f6e017fe58a1bed994

$ stat -f -c "%T" /opt
ext2/ext3
```

### 6.3 执行重启

```sh
sync; reboot
```

### 6.4 重启后

```text
$ uptime
 00:06:22  up   0:00,  0 users,  load average: 0.49, 0.00, 0.11
           ↑ uptime 归零，确认确实重启过

$ cat /opt/sgw-d25-test/marker.txt
PERSIST_TEST_341                                    ← 内容一致

$ md5sum /opt/sgw-d25-test/marker.txt
37cb145da2cb92f6e017fe58a1bed994                    ← 与重启前完全相同

$ stat -c "dev=%d ino=%i" /opt/sgw-d25-test/marker.txt
dev=45826 ino=17788                                 ← inode 也完全相同
```

**三重证据**：内容、md5、inode 全部一致，且 `uptime` 归零证明重启真实发生。
这排除了"文件没落盘只是内存缓存"的可能。

### 6.5 对照组：/tmp 确实被清空

```text
$ ls /tmp/d25-1
（不存在）
```

**对照有效**：`/opt` 保留而 `/tmp` 清空，说明测试能区分两种文件系统，
不是"什么都没变"的假阳性。

**结论**：`/opt` 是可靠持久化位置，可用于 OTA 安装目录。
`/tmp` 不可用于任何需持久的状态。

## 7. x86 回归（PASS）

```text
$ make test
test_crc16 ... test_tcp_send_timeout   全部 PASS
全部单元测试通过
```

10/10 单元测试通过，无回归。

## 8. 未解决的问题与剩余风险

| # | 项目 | 状态 | 说明 |
| --- | --- | --- | --- |
| R1 | **Gateway 运行时资源占用未采集** | **未验证** | 第 3 项验收子项缺口；需 `top` 或 `/proc/<pid>` 采样 |
| R2 | `start-stop-daemon` 是否存在 | **未确认** | 影响 D25-2 的 S5 实现方式 |
| R3 | ADB 状态曾长时间 offline | **已恢复** | 11-09 复查为 `device`，shell/push 均可用；根因未确认 |
| R4 | Gateway 无 init 脚本 | 待建 | D25-2 的 S5 需要新建 `/etc/init.d/S*` |
| R5 | 端口 7000 固定 | 已知限制 | 第二实例因冲突退出，见 D25-1 R3 |
| R6 | `manifest` 尾部多余字节被接受 | 已知限制 | 既有行为，x86/ARM 一致 |
| R7 | 未验证断电（非正常关机）场景 | 未验证 | 本轮用 `reboot`，非直接掉电 |
| R8 | 未做 OTA 完整升级流程 | 未验证 | 属 D25-2 范围 |

**R1 需要特别说明**：任务书第 3 项要求"核查……资源占用"。
本轮确认了启动方式、进程与端口，但**没有采集 CPU/内存数据**。
这是明确的缺口，不应因其他子项通过而被忽略。

**R2 说明**：本轮未执行 `command -v start-stop-daemon`。
BusyBox 通常内置该命令，但**未验证就不能假定**。

## 9. 对 D25-2 的关键输入

本轮实测直接回答了 D25-2 设计文档第 2 节列出的四个未知：

| 未知项 | D25-1B 实测答案 | 对设计的影响 |
| --- | --- | --- |
| **U1** `/opt` 是否持久 | **是**（含真实重启验证） | S7 前置满足，可用 `/opt` 作为安装根 |
| **U2** Gateway 由谁启动 | **当前无人启动** | 需先建 init 脚本，再走"外部实现"分支 |
| **U3** init 系统 | **BusyBox init**（`/etc/init.d/S*`） | 写 `S99*` 风格脚本，不用 systemd unit |
| **U4** start-stop-daemon | **未确认** | 实现前必须先查 |

### 9.1 由实测得出的设计修正建议

基于 U2/U3 的答案，D25-2 的 S5（外部 supervisor 实现）应调整为：

1. 新建 `/etc/init.d/S99smartgateway`，遵循 BusyBox 约定（`start|stop|restart`）。
2. 启动时由脚本写 pidfile 到 `/opt/<install>/run/gateway.pid`。
3. OTA Agent 通过 pidfile + `/proc` 校验来管理，而不是 `waitpid`。
4. 停止时**只发 SIGTERM**，由 init 脚本负责 wait 与回收，
   避免 OTA 与 init 争抢子进程回收。

**不要**让 OTA Agent 直接 `kill` 并 `wait` 一个非自己子进程的 Gateway——
这正是 D25-2 第 1.2 节指出的缺口（`waitpid` 只能回收自己的子进程）。

### 9.2 D25-2 的 S1–S4 现在可以推进

S1–S4（supervisor 接口、自管实现、升级流程插入 0a–0c、故障注入）
**不依赖 U1–U4 的答案**，且 `/opt` 持久性已确认，可以立即开始。
只有 S5（init 集成）需要等 R2 的 `start-stop-daemon` 确认。

## 10. 复现命令

三条通道任选其一即可，互为备份。

```sh
# ---- 通道 A：ADB（USB，最快，无需网络）----
adb devices -l                       # 期望出现 100ask_IMX6ULL  device
adb shell "uname -a"
adb push build/arm-d25/smart_gateway /tmp/d25-1/

# ---- 通道 B：SSH（网络）----
ping 192.168.10.3
ssh root@192.168.10.3
scp build/arm-d25/{smart_gateway,ota_agent,ota_verify} \
    root@192.168.10.3:/tmp/d25-1/

# ---- 通道 C：串口（备用，网络与 USB 都故障时的最后手段）----
stty -F /dev/ttyACM0 115200 raw -echo
screen /dev/ttyACM0 115200
# 提示：打开后必须先发一次回车，否则看不到 login 提示符

# ---- 板端验证（任选通道执行）----
/tmp/d25-1/smart_gateway --version
/tmp/d25-1/ota_agent verify /tmp/d25-1/pkg
timeout 6 /tmp/d25-1/smart_gateway --no-serial \
    --server 127.0.0.1 --port 19000

# ---- 持久化验证 ----
echo t > /opt/persist-test; sync; reboot
# 等待重启后
cat /opt/persist-test

# ---- adb 传输完整性校验 ----
sha256sum build/arm-d25/smart_gateway
adb shell "sha256sum /tmp/d25-1/smart_gateway"
# 两者应完全一致
```

## 11. 本轮清理与最终状态

板端测试数据已清理：

```text
$ rm -rf /opt/sgw-d25-test /tmp/d25-1
CLEANED
已清除
```

| 检查 | 结果 |
| --- | --- |
| 板端 Gateway 残留进程 | 0 |
| 板端 7000/19000 端口占用 | 0 |
| 板端测试目录 | 已删除 |
| 正式 Gateway | 全程未运行、未被干扰 |
| 板端 init 脚本 | 未被修改 |
| 板端网络配置 | 未被修改 |
