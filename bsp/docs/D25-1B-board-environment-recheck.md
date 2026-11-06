# D25-1B · 板端实测补验收

> 项目：SmartGateway-BSP / 100ASK i.MX6ULL
> 分支：`feature/bsp-imx6ull`
> 记录日期：2026-11-06
> 前置阶段：[D25-1 ARM 交叉编译与部署验收](D25-1-arm-build-deployment-validation.md)
> **本阶段结论：全部四项验收目标 BLOCKED。失败原因已定位到具体层面，并给出可执行的恢复清单。**

## 0. 结论

| # | 验收目标 | 结果 | 阻塞层级 |
| --- | --- | --- | --- |
| 1 | 恢复网络 / ADB / 串口连接并记录访问方式 | **BLOCKED** | 物理层及以上均不可用 |
| 2 | 板端验证 ARM Gateway、OTA Agent 及动态库依赖 | **BLOCKED** | 依赖第 1 项 |
| 3 | 核查正式 Gateway 启动方式、进程、资源、隔离条件 | **BLOCKED** | 依赖第 1 项 |
| 4 | 确认 OTA 安装目录可写性 + 跨重启持久性 | **BLOCKED** | 依赖第 1 项，且需重启 |

**明确声明**：本阶段**没有任何一项板端检查被执行**。因此不存在任何板端 PASS。
第 2 节记录确凿的失败证据，第 3 节给出对失败原因的推理与**推理的边界**，
第 4 节给出需要人工执行的恢复清单。

与 D25-1 相比，本轮环境**确有变化**（已接入 USB 网卡与板端 USB 枚举），
但**未达到可执行命令的程度**。这一区别很重要：不是"完全没连"，而是"连到了但走不通"。

### 0.1 第二次复测（2026-11-06 10:00，接入 USB 串口后）

用户接入 USB 转串口后再次复测，结论**不变**，但证据更完备：

| 通道 | 本轮状态 | 关键证据 |
| --- | --- | --- |
| USB 串口 | **设备出现，但零数据** | `/dev/ttyACM0`（CH340 `1a86:55d4`，`cdc_acm`）；115200 下 30 秒 0 字节 |
| 以太网 | 仍未通 | `enx6c1ff7716117` **rx_packets 仍为 0** |
| ADB | 仍 offline | 同上轮 |

**串口的关键否证**：曾一度在 19200/38400/57600 读到 48–226 字节，
但严格复测后确认那是**打开设备的瞬间噪声**，不是真实日志：

- 38400 连续监听 20 秒 → **0 字节**。
- 真实 U-Boot/Linux 日志在正确波特率下应接近 100% 可打印，
  而这批数据的可打印率只有 31%–75%，且无空格、无换行。
- 115200 下测试 DTR/RTS 四种组合，各 5 秒，**全部 0 字节**。

**这意味着串口线路本身没有把板端的 TX 送过来**，
与"波特率不对"是两回事——波特率错误会得到大量乱码，而不是持续静默。

### 0.2 三条通道同时失效的意义

串口静默 + 网卡零 RX + adbd 不响应，三者指向**同一个源**：
**板端没有在运行足够的代码去驱动它们**。

单看任一通道都能找到"线没接好"的解释，但三者同时为零，
使得"板端未正常启动"成为最简约的解释。**不过**这仍是推理，
不能排除"三个连接都没插对"这种巧合，见第 3 节的边界说明。

---

## 1. 本轮环境变化

D25-1 记录时（2026-11-06 00:13）三个通道全断。本轮（2026-11-06 09:48）出现变化：

| 项目 | D25-1 | 本轮 | 说明 |
| --- | --- | --- | --- |
| 宿主网卡 | 仅 `ens33` 192.168.44.128/24 | 新增 `enx6c1ff7716117` **192.168.10.2/24** | 与 D06/D09/D16 记录的历史网段一致 |
| 板端 USB | 无 | `1d6b:0104` **NXP 100ASK_IMX6ULL** 已枚举 | 板子有电、USB 外设控制器工作 |
| adb devices | 空 | `100ask_IMX6ULL offline` | USB 在，但 adbd 不响应 |
| 串口 | 无 `/dev/ttyUSB*` | 仍无 | 板载 USB 只暴露 ADB 接口 |

新增的 `enx6c1ff7716117` 是 **Realtek RTL8153 USB 千兆网卡**
（`lsusb`：`0bda:8153 ... USB 10/100/1000 LAN`，序列号 `0F1000001`），
属于**宿主机侧**的设备，不是板端网卡。

---

## 2. 失败证据

以下全部为 2026-11-06 09:48–09:54 实测输出。

### 2.1 以太网：链路通，但板端零流量

```text
$ ip -br -4 addr
ens33            UP   192.168.44.128/24
enx6c1ff7716117  UP   192.168.10.2/24

$ ethtool enx6c1ff7716117
        Speed: 100Mb/s
        Duplex: Full
        Auto-negotiation: on
        Link detected: yes

$ cat /sys/class/net/enx6c1ff7716117/carrier
1
```

物理层协商成功。但收发计数是决定性的：

```text
rx_packets  = 0
rx_bytes    = 0
tx_packets  = 143
rx_errors   = 0
rx_crc_errors = 0
tx_errors   = 0
```

**TX 有 143 个包发出，RX 恒为 0，且无 CRC 错误。**

持续观察 10 秒，`rx_packets` 保持 `0 -> 0`。

### 2.2 全网段扫描无主机

```text
$ (并发 ping 扫描 192.168.10.1..20, 100..105, 200)
  UP: 192.168.10.2        ← 只有宿主机自己

$ ip neigh show dev enx6c1ff7716117
192.168.10.1   FAILED
192.168.10.3   FAILED     ← D06/D09/D16 记录的板端地址
...
（全部 FAILED）
```

`192.168.10.3` ping 丢包 100%，SSH 22 端口不可达。
另扫描 `192.168.44.0/24` 也无板端（仅网关 `.1`/`.2`/`.254`）。

### 2.3 ADB：USB 枚举成功，协议握手无响应

USB 设备本身完全正常：

```text
$ lsusb
Bus 003 Device 005: ID 1d6b:0104 Linux Foundation Multifunction Composite Gadget

$ lsusb -v -d 1d6b:0104
  iManufacturer   1 NXP
  iProduct        2 100ASK_IMX6ULL
  serial            100ask_IMX6ULL
  bNumInterfaces    1
  bConfigurationValue 1
  speed             480          (USB 2.0 High Speed)
  authorized        1

$ 接口
  bInterfaceClass     255 Vendor Specific Class
  bInterfaceSubClass   66 [unknown]        ← ADB 接口特征
  端点: ep_01 (Bulk out), ep_81 (Bulk in)
```

`adb devices` 显示 `offline`。用 `ADB_TRACE` 观察协议层：

```text
I adb: 100ask_IMX6ULL: to remote: [CNXN] arg0=0x1000001 arg1=0x100000 ... host::features=...
D adb: [ usb read 512 fd = 12]
D adb: [ reap urb - wait ]
（5 秒无任何来自设备的数据）
I adb: 100ask_IMX6ULL: connection terminated: write failed
D adb: [ usb read 512 ] = -1
D adb: ERROR: n = -1, errno = 22 (Invalid argument)
I adb: 100ask_IMX6ULL: read failed: Invalid argument
D adb: transport: 100ask_IMX6ULL destroy (kicking and closing)
（随后无限重试同样的循环）
```

`urbnum` 从 169 增长到 171，说明主机在持续重试，设备端从不回应。

已尝试且**均无效**的恢复手段：

- `adb kill-server` / `adb start-server` / `adb reconnect offline` / `adb reconnect device`
- `adb usb` → 报 `error: device offline`
- `adb wait-for-device` 等待 40 秒 → 超时
- 检查 `~/.android/adbkey` 存在（指纹 `58C96BBF...`），非"未授权"问题
- `/dev/bus/usb/003/005` 权限 `crw-rw-rw-`，非权限问题

### 2.4 串口：设备在，但零数据（第二轮复测）

**第一轮（09:48）**：无任何串口设备。

**第二轮（10:00，用户接入 USB 转串口后）**：设备出现。

```text
$ ls -l /dev/ttyACM0
crw-rw---- 1 root dialout 166, 0 Nov  6 10:00 /dev/ttyACM0

$ ls -l /dev/serial/by-id/
usb-1a86_USB_Single_Serial_5B8E649140-if00 -> ../../ttyACM0

$ lsusb -v -d 1a86:
  idVendor         0x1a86   (WCH)
  idProduct        0x55d4   USB Single Serial
  iSerial          5B8E649140

$ lsusb -t
  |__ Port 001: Dev 006, If 0, Class=Communications, Driver=cdc_acm
  |__ Port 001: Dev 006, If 1, Class=CDC Data,      Driver=cdc_acm
```

设备识别为 CH340 系列 CDC-ACM，驱动绑定正常（`cdc_acm`），
`/dev/ttyACM0` 存在，用户 `ubuntu` 在 `dialout` 组，可读写。

**但是读不到任何数据。**

| 测试 | 结果 |
| --- | --- |
| 115200 被动监听 8 秒 | 0 字节 |
| 115200 监听 30 秒（每 3 秒发 `\r\n` 唤醒） | 0 字节 |
| DTR/RTS 四种组合 × 各 5 秒 | 全部 0 字节 |
| 波特率扫描 9600→2000000（12 档） | 115200/230400/460800/921600/1500000 等均 0 字节 |
| DTR 脉冲尝试触发复位 | 0 字节 |
| `TIOCMGET` 线路状态 | `CTS=1`（线路有载波指示），但无数据 |

**唯一"有数据"的假象已排除**：波特率扫描时 19200/38400/57600 各读到
48/226/180 字节，一度看似有希望。但严格复测否证了它：

```text
$ (38400 连续监听 20 秒)
总字节: 0
→ 之前的 226 字节是打开设备瞬间的线路噪声
```

判据有三条，都指向"噪声"而非"错波特率的真实日志"：

1. **真实日志在错波特率下会是大量乱码，但不会"一次之后就彻底静默"**。
   波特率不匹配是持续性的，每一行输出都会产生乱码。
2. **可打印率只有 31%–75%**，而正常 U-Boot/Linux 日志接近 100%。
3. **统计不到空格 `0x20` 与换行 `0x0a`**——真实内核日志必然大量包含这两个字节。

**因此结论是：串口 TX 线路上根本没有来自板端的数据**，
问题不在波特率，而在**线路或板端本身**。

---

## 3. 原因分析与推理边界

### 3.1 可以确定的事实

1. **板子有电且 USB 外设控制器工作**。USB 枚举（描述符、配置、端点）
   由板端 UDC + gadget 驱动完成，不依赖用户态 adbd。枚举成功说明
   这部分正常。
2. **adbd 没有完成 CNXN 握手**。主机发出 CNXN 后 5 秒无响应，
   随后写入失败。典型原因：adbd 未运行、adbd 卡死、或用户态根本没起来。
3. **板端以太网从未发出任何一帧**。RX 恒 0 且无 CRC 错误，
   说明不是"收到了但校验失败"，而是**一个字节都没收到**。
4. **宿主机无 root，无法用原始套接字做链路层验证**
   （`socket(AF_PACKET)` → `PermissionError`），因此不能进一步区分
   "对端没发" 与 "对端发了但被硬件丢弃"。

### 3.2 可能的原因（按可能性排序，**均为推测**）

| # | 推测 | 支持点 | 反驳点 |
| --- | --- | --- | --- |
| A | 板端未上电或未正常启动到用户态（停在 U-Boot / 内核 panic / 卡在启动） | adbd 不响应；网卡零流量；**串口零字节** | USB gadget 能枚举 —— 但 U-Boot 也可能枚举 USB |
| B | 串口模块 TXD/RXD 未正确接到板端（缺 GND、未交叉、接错引脚） | 串口零字节 | 若板端未启动，接对了也静默 → 两者无法区分 |
| C | 网线未接到板端（接到了交换机/空口） | RX 恒 0 | 若板端未启动，接对了也零流量 → 无法区分 |
| D | adbd 崩溃但内核与网络正常 | USB 枚举 + 网卡可能 up | 若内核正常且 eth0 up，不该零 RX；**且串口应有日志** |
| E | VMware USB/网络直通不稳定 | 曾出现 offline | 串口设备与网口都能正常枚举，不太像 |

**D 已被显著削弱**：如果内核正常运行，串口至少应输出启动日志。
串口零字节使得"内核正常但 adbd 崩了"这个假设很难成立。

**A 与 B/C 仍无法区分**，因为它们预测同一现象（全静默）。
区分方法只有一个：**看板端物理指示灯，并按复位键观察串口**（见第 4 节第 1 步）。

### 3.3 推理的边界（必须说明）

我**不能**从这个证据推断"板子坏了"或"系统损坏"。

零字节串口 + 零 RX 网络 + adbd 无响应，这组现象同样可以由
**一根没接好的线（串口少接 GND，或网线没插）** 造成。这两个原因
指向完全不同的处置方式：

- 若板端真没启动 → 需要检查供电、按复位、可能需要恢复系统。
- 若只是线没接对 → 插好线即可，一分钟解决。

因此第 4 节把"**确认板端是否在上电运行**"和"**串口回环测试**"
放在最前面，而不是建议刷机。

**特别提醒**：本轮已排除"波特率不对"这一常见误判。
波特率错误会得到**持续的大量乱码**，而实测是**持续静默**。
这两个现象指向不同的原因，不能混为一谈。

---

## 4. 需要人工执行的恢复清单

我无法执行这些操作（需要物理接触、root 权限或重新上电）。请按顺序执行。

> **2026-11-06 10:00 更新**：USB 转串口已接入，`/dev/ttyACM0` 可用，
> 但**读不到任何数据**（详见 2.4）。因此"接串口看日志"这一步已经做过，
> 结论是**串口线路上没有板端发来的数据**。
> 下一步的重心应转向**板端供电与启动状态**，而不是继续调串口参数。

### 第 1 步：确认板端是否真的在上电运行

这是现在最关键的一步。三条通道同时为零，最可能的解释是板端没跑到能驱动它们。

1. **看板端电源指示灯是否亮**。不亮 → 直接检查电源适配器与开关。
2. **看板端网口指示灯**（插着网线时）：
   - 不亮 → 板端 PHY 未工作，通常说明**板端未启动或未上电**
   - 亮 → 板端至少供电正常，继续
3. **按一下板端复位键**，同时观察串口是否有任何输出：

```sh
# 宿主机执行，然后按板端复位键
python3 -c "
import serial,time
s=serial.Serial('/dev/ttyACM0',115200,timeout=0.2)
s.dtr=False; s.rts=False
t0=time.time(); buf=b''
while time.time()-t0<30:
    d=s.read(4096)
    if d: buf+=d; print(repr(d[:80]),flush=True)
print('总计',len(buf),'字节')
"
```

| 复位后串口现象 | 含义 | 下一步 |
| --- | --- | --- |
| 出现 `U-Boot 2017.03` | 板端正常启动 | 看是否停在 `=>`；若停则输入 `boot` |
| 出现 `Starting kernel` 后无输出 | 内核启动失败 | **收集完整日志，勿刷机** |
| 出现 `login:` | 用户态正常 | 转第 3 步配网络 |
| **仍然 0 字节** | 串口线未连到板端 TX，或板端未启动 | 见第 2 步 |

### 第 2 步：检查串口接线（若第 1 步仍零字节）

CH340 这类 USB 转串口模块，**必须交叉连接**：

```text
模块 TXD  ──→  板端 RXD
模块 RXD  ←──  板端 TXD
模块 GND  ───  板端 GND      ← 最容易漏接
```

常见错误：

- **只接了 TX/RX 没接 GND** → 表现为完全静默或间歇噪声，与本轮现象一致。
- **TX/RX 没有交叉**（TXD 接 TXD）→ 板端收不到，主机也读不到。
- **接错排针**（接到板端其他 UART，如蓝牙/WiFi 串口）→ 该口可能无输出。
- **电平不匹配**：i.MX6ULL 调试口是 **3.3V TTL**，若模块是 RS-232 电平
  （±12V）则既不工作还可能损坏。CH340 常见有 3.3V/5V 版本，需确认跳线在 3.3V。

确认方法：把模块的 **TXD 与 RXD 短接**（回环测试），然后发送字符：

```sh
python3 -c "
import serial,time
s=serial.Serial('/dev/ttyACM0',115200,timeout=0.5)
s.write(b'LOOPTEST\r\n'); time.sleep(0.3)
print('回环收到:', s.read(100))
"
```

- 能收到 `LOOPTEST` → **模块本身正常**，问题在板端或接线。
- 收不到 → **模块或驱动有问题**，先解决这个。

### 第 3 步：确认板端启动介质

若串口能看到 U-Boot，检查启动是否正常：

```text
=> mmc list
=> printenv bootcmd
=> boot
```

**不要执行 `saveenv`、`mmc write`、`mmc erase`**（D01 已明确禁止，
在恢复方案未验证前不做破坏性操作）。

### 第 4 步：板端网络配置（进入系统后）

```sh
ip -br link                      # eth0 是否 UP
ip -br -4 addr                   # 是否有 192.168.10.3
ip route
```

若 `eth0` 为 DOWN：

```sh
ip link set eth0 up
ip addr add 192.168.10.3/24 dev eth0    # 与宿主机 192.168.10.2 同网段
```

若需要持久化，**先确认**板端用的是 Buildroot 的 `/etc/network/interfaces`
还是 `S*network` 脚本，**不要**直接假定。

### 第 5 步：恢复 adb（可选，串口优先）

```sh
ps | grep adbd           # adbd 是否在运行
adbd &                   # 若未运行，手动拉起
```

注意：Buildroot 的 adbd 可能只在特定 USB 配置下启动。
若串口可用，**优先用串口**，不必纠结 adb。

### 第 6 步：回报结果

连接恢复后，执行：

```sh
sh bsp/tools/d25-1-arm-check.sh target
```

把输出贴回，即可补齐 D25-1B 的四项验收。

---

## 5. 已就绪、等待板端的资产

以下内容在 D25-1 已完成，**一旦连接恢复即可直接使用**，无需返工：

| 资产 | 路径 | 状态 |
| --- | --- | --- |
| ARM Gateway | `build/arm-d25/smart_gateway` | 已构建，SHA-256 `1beb5d2f...` |
| ARM OTA Agent | `build/arm-d25/ota_agent` | 已构建，SHA-256 `876ddf5d...` |
| ARM ota_verify | `build/arm-d25/ota_verify` | 已构建，SHA-256 `961a0a5e...` |
| 板端核验脚本 | `bsp/tools/d25-1-arm-check.sh` | 已就绪，`target` 模式覆盖四项验收 |
| 静态兼容性证据 | D25-1 第 5 节 | glibc 需求 ≤ 2.28 < 板端 2.30 |

板端恢复后执行的最短路径：

```sh
# 宿主机
scp build/arm-d25/{smart_gateway,ota_agent,ota_verify} root@192.168.10.3:/tmp/d25-1/
scp bsp/tools/d25-1-arm-check.sh root@192.168.10.3:/tmp/

# 板端
sh /tmp/d25-1-arm-check.sh target /tmp/d25-1/ota_agent
/tmp/d25-1/smart_gateway --version
```

### 5.1 顺带完成 D25-2 的板端前置验证

D25-2（进程生命周期管理）的 pid 校验算法已在 x86 与 ARM 上各通过 9 项自测
（见 [D25-2 设计 4.2.2](D25-2-process-lifecycle-design.md)），但**板端内核
4.9.88 的 `/proc/<pid>/stat` 格式尚未在该板上确认**。

板端恢复后，可一并验证（成本几乎为零）：

```sh
# 宿主机交叉编译（与 D25-1 相同的工具链）
arm-buildroot-linux-gnueabihf-gcc -std=c11 -O2 -Wall -Wextra -Werror \
    -mcpu=cortex-a7 -mfloat-abi=hard -mfpu=neon-vfpv4 \
    --sysroot=$SYSROOT \
    -o /tmp/pidsig-arm bsp/tools/d25-2-pidsig.c

scp /tmp/pidsig-arm root@192.168.10.3:/tmp/

# 板端
/tmp/pidsig-arm selftest
```

**期望**：9 项全部 PASS，且第 7 项显示"两者相同"。
若第 1 项 `starttime == 0`，说明板端 `/proc` 格式与预期不同，
D25-2 的 S1 需改用备用方案（见设计文档 R3）。

再实际检视一个真实进程：

```sh
/tmp/pidsig-arm inspect $$
```

---

## 6. 对本阶段结论的自我限制

- 本文档中**没有**任何"PASS"。第 2 节全部是失败证据，第 3 节全部是推测。
- 第 3.2 节的 A–E 是假设，**不是**诊断结论。未经串口日志确认，
  不能对外说"板子系统崩了"。
- 第 4 节的恢复清单是建议，其有效性未经验证（我无法执行）。
- 第 5 节的资产可复现性已在 D25-1 验证，但**未在板端运行过**。
