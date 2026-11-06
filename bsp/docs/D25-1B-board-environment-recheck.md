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

### 2.4 串口：不存在

```text
$ ls /dev/ttyUSB* /dev/ttyACM*  → 无
$ ls /dev/serial/by-id/         → 无
$ (USB gadget 接口列表)
  net 接口                    → 无（仅 1 个 Vendor Specific 接口）
```

板载 USB 只导出 ADB 一个接口，**没有 RNDIS/ECM 网络接口，也没有 CDC-ACM 串口**。
因此无法通过 USB 获得串口控制台。

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
| A | 板端系统未正常启动到用户态（停在 U-Boot、内核 panic、或卡在启动） | adbd 不响应；网卡零流量 | USB gadget 能枚举 —— 但 U-Boot 也可能枚举 USB |
| B | 网线未接到板端（接到了交换机/空口） | RX 恒 0 | 无法验证，需人工看指示灯 |
| C | 板端 `eth0` 未 up 或未配 IP | RX 恒 0 | 若内核已起，即使无 IP 也应有 IPv6 DAD/MLD 帧，实测无 |
| D | adbd 崩溃但内核正常 | USB 枚举 + 网卡可能 up | 若内核正常且 eth0 up，不该零 RX |
| E | VMware USB/网络直通不稳定 | 曾出现 offline | 设备描述符可正常读取，不太像 |

**推测 C 的反证值得注意**：如果板端 Linux 已启动且 `eth0` 处于 UP，
即使没有配置 IPv4，内核也会发送 IPv6 链路本地地址的 DAD（邻居请求）和
MLD 报文。实测 RX=0 说明**连这些都没有**。这提高了 A（未到用户态）的权重，
但**不能排除** B（线没接对）。

### 3.3 推理的边界（必须说明）

我**不能**从这个证据推断"板子坏了"或"系统损坏"。零 RX 同样可以由
**一根没插好的网线**造成。这两个原因指向完全不同的处置方式：
前者需要重新上电/恢复系统，后者只需插好线。

因此第 4 节把"人工确认物理连接"放在第一位，而不是建议刷机。

---

## 4. 需要人工执行的恢复清单

我无法执行这些操作（需要物理接触、root 权限或重新上电）。请按顺序执行。

### 第 1 步：确认物理连接（最可能、成本最低）

1. 确认板端网口与 `enx6c1ff7716117`（RTL8153）之间是**直连网线**
   （或经交换机），**不是**接到别的设备。
2. 看板端网口指示灯：**是否亮/闪**。
   - 不亮 → 线或板端 PHY 问题，优先换线。
   - 亮但不闪 → 链路通但无数据，继续第 2 步。
3. 若方便，把板端串口线接到 PC，用 `picocom -b 115200 /dev/ttyUSB0`
   直接看启动日志 —— 这是唯一能确定"板端到底停在哪个阶段"的手段。

### 第 2 步：确认板端状态

若串口可用，观察：

```text
U-Boot 2017.03 ...            ← 停在 U-Boot 提示符？
Starting kernel ...           ← 卡在这？
... login:                    ← 已到用户态
```

| 观察 | 含义 | 下一步 |
| --- | --- | --- |
| 停在 `=>` 提示符 | 未启动内核 | 板端执行 `boot` |
| 停在 `Starting kernel` 后无输出 | 内核启动失败 | 收集完整日志，勿刷机 |
| 出现 `login:` | 用户态正常 | 检查 `ifconfig -a` 与 `adbd` |
| 完全无输出 | 串口线或供电问题 | 检查线序、供电 |

### 第 3 步：板端网络配置（进入系统后）

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

### 第 4 步：恢复 adb（可选，串口优先）

```sh
ps | grep adbd           # adbd 是否在运行
adbd &                   # 若未运行，手动拉起
```

注意：Buildroot 的 adbd 可能只在特定 USB 配置下启动。
若串口可用，**优先用串口**，不必纠结 adb。

### 第 5 步：回报结果

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
