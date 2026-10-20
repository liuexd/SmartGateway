# D08 · Device Tree GPIO 资源化与实机端到端验收

> 验收日期：2026-10-15（开发者终端记录）
>
> 分支：`feature/bsp-imx6ull`
>
> 关联阶段：[D06 GPIO 端到端验证](D06-gpio-end-to-end-validation.md) · [D07 GPIO 可靠性验证](D07-gpio-reliability-validation.md)
>
> 设备树源码：[`bsp/dts/d08-imx6ull-sgw-gpio.dts`](../dts/d08-imx6ull-sgw-gpio.dts)
>
> 驱动源码：[`bsp/drivers/sgw_gpio_event/sgw_gpio_event.c`](../drivers/sgw_gpio_event/sgw_gpio_event.c)
>
> **验收结论**：D08 离线 DTB 构建、U-Boot 临时加载实验 DTB、Linux OF Platform Device/Driver 匹配、GPIO Descriptor 获取、pinctrl 引脚复用、IRQ 与环形事件队列，以及三次真实按键到 NODE03 Server 的 6 条 EDGE 事件均有终端日志支持并通过。**没有覆盖原厂 DTB，已完成普通重启后的原有设备树节点缺失、DTB 哈希和根分区核对；zImage 在重启后尚未重新计算哈希。不宣称量化消抖、异常路径/持有 FD 卸载、长期可靠性或持久化部署已通过。**

## 1. 平台、资源与改造范围

- 开发板：100ASK i.MX6ULL 14x14，Linux 4.9.88 / ARMv7 / Buildroot；USB-TTL 串口可进入 U-Boot。
- GPIO：Linux GPIO115 = GPIO4_IO19 = `MX6UL_PAD_CSI_VSYNC`，按键物理电平低为 PRESSED，高为 RELEASED。
- PAD 寄存器只读观察：MUX `0x020E01DC = 0x00000005`（ALT5 / GPIO）；PAD control `0x020E0468 = 0x000070B0`。
- D08 DTS 使用 `pinctrl-0 = <&pinctrl_sgw_button>`、`MX6UL_PAD_CSI_VSYNC__GPIO4_IO19 0x70b0` 和 `button-gpios = <&gpio4 19 GPIO_ACTIVE_LOW>`，`compatible = "smartgateway,sgw-gpio-event"`。
- 驱动从硬编码 GPIO / 软件创建 Platform Device 改为 `devm_gpiod_get(&pdev->dev, "button", GPIOD_IN)`、`gpiod_to_irq()` 与 `of_match_table`；事件协议仍报告原始物理 LEVEL（0=按下、1=释放），避免 GPIO_ACTIVE_LOW 的逻辑电平变换破坏用户态 ABI。
- 保留 20ms 延迟工作消抖、64 容量环形队列、`poll/read`、`/dev/sgw_event0` 和 sysfs `stats`。

## 2. DTB 构建与部署（PASS）

离线完成 `cpp` 预处理、`dtc` 编译和反编译核对；实验 DTB 大小为 37605 字节。编译时有 dtc 警告，未在本阶段逐条证明全部是原厂 DTS 历史警告；不能将其表述为“零警告”。

实验文件单独命名并上传至既有 eMMC 根文件系统：

| 文件 | SHA-256 | 结论 |
| --- | --- | --- |
| Ubuntu `/tmp/d08-imx6ull-sgw-gpio.dtb` → 板端 `/boot/sgw-d08-test.dtb` | `b71deb9051d6e0a84b2ec0f680078b43f2a679cc0db9c375a7c67df9a6edb916` | 两端一致，PASS |
| Ubuntu `bsp/drivers/sgw_gpio_event/sgw_gpio_event.ko` → 板端 `/root/sgw_gpio_event_d08.ko` | `246b8d7a64f6fe6587ae098e5c2c31045033c63e3c99ddd394a421da4b0ab079` | 两端一致，PASS |
| 原厂 `/boot/100ask_imx6ull-14x14.dtb` | `daf7317e99a440fc34622c8f2bbf1a5a003248a926d9436700c840e67256813f` | 上传前后不变，PASS |
| 原厂 `/boot/zImage`（上传前基线） | `4370d3e8dd70fbf51a6aea8aa583e96704f97e33d537b2b1b336fa9e967a82d5` | 已记录基线；未提供上传后再次计算的日志 |

**安全边界**：没有替换原有 DTB/zImage，也没有执行 `saveenv`；实验 DTB 以独立文件存在于 `/boot`。

## 3. U-Boot 启动链确认与实验 DTB 实机加载（PASS）

U-Boot 2017.03-g8ba4c5bb19-dirty；环境变量：

```text
mmcdev=1
mmcpart=2
bootdir=/boot
image=zImage
fdt_file=100ask_imx6ull-14x14.dtb
loadaddr=0x80800000
fdt_addr=0x83000000
mmcroot=/dev/mmcblk1p2 rootwait rw
tee=no
boot_fdt=try
loadimage=ext2load mmc ${mmcdev}:${mmcpart} ${loadaddr} ${bootdir}/${image}
loadfdt=ext2load mmc ${mmcdev}:${mmcpart} ${fdt_addr} ${bootdir}/${fdt_file}
```

`mmc list` 确认 U-Boot `MMC1 (eMMC)`；`ext4ls mmc 1:2 /boot` 能列出原厂 DTB、zImage。启动的实际实验流程采用原有 zImage + 新 DTB：

```text
run loadimage
ext2load mmc 1:2 0x83000000 /boot/sgw-d08-test.dtb
fdt addr 0x83000000
fdt print /sgw-gpio-event
run mmcargs
bootz 0x80800000 - 0x83000000
```

实验 DTB 从 eMMC 读取的实际输出为 `37605 bytes read in 67 ms`；RAM 中解析：

```dts
sgw-gpio-event {
    compatible = "smartgateway,sgw-gpio-event";
    pinctrl-names = "default";
    pinctrl-0 = <0x0000003f>;
    button-gpios = <0x00000009 0x00000013 0x00000001>;
    status = "okay";
};
```

`0x13=19`（GPIO4_IO19），`0x01` 表示 `GPIO_ACTIVE_LOW`。Linux 成功启动并进入 Shell，`/proc/device-tree/sgw-gpio-event` 存在，读取 compatible 得到 `smartgateway,sgw-gpio-event`。**这是临时 RAM 启动实验，不是永久启用自定义 DTB。** 尤其不要在加载新 DTB 后执行 `run mmcboot`，它会通过原 `loadfdt` 重新装入厂商 DTB。

## 4. OF 匹配、驱动 probe 与 sysfs（PASS）

Linux 首次检查 `/proc/modules | grep sgw_gpio_event` 无输出；手动执行 `insmod /root/sgw_gpio_event_d08.ko` 后内核输出：

```text
sgw_gpio_event: module init
sgw-gpio-event sgw-gpio-event: probe: start
sgw-gpio-event sgw-gpio-event: initial stable level=1 debounce=20ms
sgw-gpio-event sgw-gpio-event: misc device /dev/sgw_event0 ready
sgw-gpio-event sgw-gpio-event: IRQ 194 ready, queue capacity=64
sgw_gpio_event: platform driver registered
```

`probe` 的打印早于注册完成提示，是 `platform_driver_register()` 期间可同步匹配并执行 probe 的正常行为。

- `/dev/sgw_event0` 创建成功：字符设备 major=10、minor=56。
- sysfs 的 OF Platform Device 实际位置为 `/sys/devices/soc0/sgw-gpio-event`，稳定查询入口 `/sys/bus/platform/devices/sgw-gpio-event/stats`；不能复用 D07 软件创建设备的 `/sys/devices/platform/sgw-gpio-event/stats` 路径。
- 板端模块文件重命名为 `sgw_gpio_event_d08.ko`，但内核模块内部名称仍是 `sgw_gpio_event`；`rmmod /root/sgw_gpio_event_d08.ko` 的失败不代表驱动未加载。不要仅依据文件名推断模块名。
- 本次验证的是 **OF 匹配和自动调用 probe**；内核模块本身仍由 `insmod` 手动加载，并未验证模块自动装载。

## 5. pinctrl、GPIO IRQ 与驱动队列（PASS）

板端：

```text
gpiochip3: GPIOs 96-127, parent: platform/20a8000.gpio, 20a8000.gpio:
 gpio-115 (                    |button              ) in  hi IRQ

pin 119 (MX6UL_PAD_CSI_VSYNC): sgw-gpio-event (GPIO UNCLAIMED) function imx6ul-evk group sgwbuttongrp
```

`sgw-gpio-event` 及 `sgwbuttongrp` 显示 pinctrl mux 所属设备和功能组；此处的 `GPIO UNCLAIMED` 是 pinctrl 调试字段，不能覆盖 gpiolib 中 GPIO115 已申请、已设置为输入 IRQ 的证据。

无人消费事件时，执行一次按下/释放，`stats` 由初始全零变成：

```text
irq_total=2
enqueued=2
dropped=0
depth=2
capacity=64
head=2
tail=0
last_sequence=2
last_level=1
last_timestamp_ns=391830275045
```

结论：本次机械按键产生两个稳定电平转换事件，入队 2、丢弃 0；未通过可控波形精确测量 20ms 消抖阈值。

## 6. Agent → Gateway → Server 实机回归（PASS，本次三次按键样本）

- GPIO Agent：板端 `/tmp/gpio_agent`；启动输出 `device=/dev/sgw_event0 gateway=127.0.0.1:7000 heartbeat=10000ms`。
- Gateway 未运行时曾出现 `connect Gateway: Connection refused`；后续恢复通信，Server 收到 TCP 客户端 `192.168.10.3:47476`，并记录 NODE03 的 `KIND=HEARTBEAT` 与 `[ONLINE]` 状态。
- 测试过程中连续按下并释放物理按键 **3 次**。Server 对每次物理状态变化收到 `KIND=EDGE`（并非 `KIND=EVENT`），`DEV=4`、`GPIO=115`：

| 按键动作 | Server 消息 seq | EVT_SEQ | LEVEL | STATE | TS_NS |
| --- | ---: | ---: | ---: | --- | ---: |
| 第 1 次按下 | 17 | 7 | 0 | PRESSED | 1389240711164 |
| 第 1 次释放 | 18 | 8 | 1 | RELEASED | 1389430312498 |
| 第 2 次按下 | 19 | 9 | 0 | PRESSED | 1390260214498 |
| 第 2 次释放 | 20 | 10 | 1 | RELEASED | 1390440215498 |
| 第 3 次按下 | 21 | 11 | 0 | PRESSED | 1390920267498 |
| 第 3 次释放 | 22 | 12 | 1 | RELEASED | 1391100129165 |

Server `NodeStore` 六条日志均显示 `node=NODE03`、`dev=GPIO_BUTTON`，`EVT_SEQ=7..12` 连续、按下/释放顺序正确。

同一轮按键前后内核 stats 对比：

| 字段 | 按键前 | 三次按键后 | 增量 |
| --- | ---: | ---: | ---: |
| irq_total | 26 | 32 | +6 |
| enqueued | 6 | 12 | +6 |
| dropped | 0 | 0 | 0 |
| depth | 0 | 0 | 0 |
| head | 6 | 12 | +6 |
| tail | 6 | 12 | +6 |
| last_sequence | 6 | 12 | +6 |
| last_level | 1 | 1 | 释放态 |

最终 `last_timestamp_ns=1391100129165` 与 Server EVT_SEQ=12 的 `TS_NS` 一致。

**判定**：本轮 3 次按下/释放对应的 6 个有效事件均从驱动队列被消费并出现在 Server，未发生驱动环形队列丢弃。本结果不构成网络故障、断线重传、应用层 Exactly-Once 或长期零丢包保证。

## 7. 测试汇总与范围

| 检查项 | 结果 | 证据/限制 |
| --- | --- | --- |
| GPIO 寄存器 / 引脚资源核查 | PASS | GPIO115 / CSI_VSYNC / ALT5 / PAD 0x70b0 |
| D08 DTS / DTB 离线编译与反编译 | PASS | 编译成功、节点及 pinctrl phandle 正确；历史警告来源未逐条确认 |
| OF 驱动模块编译、modinfo alias | PASS | `of:N*T*Csmartgateway,sgw-gpio-event*`、vermagic 4.9.88 |
| 独立 DTB 文件部署及 SHA-256 | PASS | 两个实验文件比对一致；原厂 DTB 哈希不变 |
| U-Boot 独立 DTB 加载与 fdt print | PASS | 37605 bytes，compatible/button-gpios 正确 |
| Linux DT 实际生效与 OF probe | PASS | /proc/device-tree 节点存在，probe 成功 |
| pinctrl / GPIO Descriptor / IRQ / misc / stats | PASS | sgwbuttongrp、GPIO115、IRQ194、/dev/sgw_event0 |
| 单次按下/释放事件入队 | PASS | enqueued=2、depth=2、dropped=0 |
| NODE03 心跳与 TCP 通信 | PASS | Server 端 NODE03 ONLINE |
| 三次按键端到端事件对照 | PASS（本次样本） | EVT_SEQ 7–12 连续，统计增量 +6 |
| 普通重启后恢复原有设备树环境 | **PASS（限定范围）** | Linux 正常进入 Shell；运行时 D08 节点不存在、原 DTB SHA-256 不变、root 为 mmcblk1p2；重启后 zImage SHA-256 尚未补验 |
| D08 驱动失败路径、并发/卸载竞态全面审查 | **NOT VERIFIED** | 功能测试不能取代静态审查与错误注入 |
| 消抖 20ms 定量精度与 perf/GDB 性能分析 | **NOT VERIFIED** | 后续阶段单独进行 |
| D08 DTB 永久安装/模块自动加载 | **NOT ATTEMPTED** | 采取临时启动，不改默认启动配置 |

## 8. 恢复原厂启动的操作与回滚验收（PASS：限定范围）

**目的**：确认本次 D08 仅为临时 DTB 启动、没有损坏既有启动路径；保留 D08 实验文件和源码，不要求覆盖、删除文件或重刷 eMMC。

1. 保存本次测试输出；在实验 Linux 中执行 `sync`，再执行 `reboot`。
2. 保持 U-Boot 原有环境不变，不使用 `saveenv`、`mmc write`、`mmc erase`，**不修改** `/boot/100ask_imx6ull-14x14.dtb` 或 `/boot/zImage`；让系统按原先的 `bootcmd` 自动启动（先前记录 `fdt_file=100ask_imx6ull-14x14.dtb`）。
3. 已按正常启动流程重新进入 Linux，执行只读检查：

```sh
cat /proc/cmdline
if [ -d /proc/device-tree/sgw-gpio-event ]; then echo 'D08 node still present: investigate boot DTB'; else echo 'D08 node absent: consistent with original DTB'; fi
sha256sum /boot/100ask_imx6ull-14x14.dtb
sha256sum /boot/zImage
```

4. 开发者本次实际输出如下：

```text
[root@100ask:~]# ls /proc/device-tree/sgw-gpio-event
ls: cannot access '/proc/device-tree/sgw-gpio-event': No such file or directory
[root@100ask:~]# cat /proc/cmdline
console=ttymxc0,115200 root=/dev/mmcblk1p2 rootwait rw
[root@100ask:~]# sha256sum /boot/100ask_imx6ull-14x14.dtb
daf7317e99a440fc34622c8f2bbf1a5a003248a926d9436700c840e67256813f  /boot/100ask_imx6ull-14x14.dtb
```

检查原厂 DTB 哈希应仍为 `daf7317e99a440fc34622c8f2bbf1a5a003248a926d9436700c840e67256813f`；zImage 与上文基线比较。没有新节点加上校验不变可支持“恢复原有启动路径”，但并非逐字节核查 RAM 中 FDT 的完整证据。
5. 以上日志支持：重启后的系统运行时不包含 D08 节点，原 DTB 未发生字节变化，启动参数与原基线一致；**没有提供重启后的 zImage SHA-256，也没有通过 U-Boot 启动日志逐字节比对 RAM 中的 FDT**。这是一项限定范围内的回滚 PASS，不是完整的镜像恢复能力或掉电容错测试。
6. 如以后需要再测试 D08，只需通过 UART 手动从独立路径加载实验 DTB 到 RAM；**不要把实验版改名覆盖原版，也不要把测试用 `bootcmd` 永久保存**。

此前已验证的 U-Boot 默认 `fdt_file=100ask_imx6ull-14x14.dtb`；结合重启后 Linux 运行时节点缺失和原厂 DTB 哈希不变，本节记录为 **PASS（限定范围）**。重启后 zImage 哈希复核仍未完成。

## 9. 开发与 Git 归档注意事项

- 驱动源码的改造记录与本文件属于 D08；`bsp/apps/input_key_test/` 是开发者工作区中的独立未跟踪目录，不应混入 D08 提交。
- 因为本地仓库与 GitHub 远端同步可能有时序差异，**以实际执行 `git fetch`、`git status -sb`、`git log` 和 GitHub 远端记录确定是否已推送**，不能将 `git commit` 自动等同于远端推送。
- 下阶段 D09 可专门开展失败路径代码审查、GDB/perf 调试及性能基线采集。
