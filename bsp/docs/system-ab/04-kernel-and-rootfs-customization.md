# Kernel and Buildroot RootFS Customization Plan

## 1. 目标

建立可复现的 i.MX6ULL 专用 Kernel 和 RootFS 构建流程，再逐步裁剪配置。不得一开始就修改内核版本、工具链和板级 DTS 多个变量。

## 2. Kernel 基线复现

- 获取与板端 Linux 4.9.88 完全匹配的厂商源码、补丁和 defconfig。
- 记录源码 commit、补丁摘要、编译器版本和构建命令。
- 使用项目记录的 Linaro GCC 6.2.1 工具链复现 zImage、DTB 和模块。
- 保存原始 .config，生成可审查的配置差异。
- 首先证明原始配置可构建，再进行裁剪。

## 3. 必须保留的功能依赖

| 子系统 | 保留理由 |
|---|---|
| i.MX6ULL SoC / clock / pinctrl | 板级启动与引脚复用 |
| MMC/eMMC | 从 eMMC 加载和挂载根文件系统 |
| ext4 | 当前根文件系统 |
| GPIO / IRQ | GPIO 事件驱动 |
| I2C | AP3216C 传感器 |
| USB gadget / FunctionFS | ADB 调试通道 |
| WiFi 所需总线和驱动 | 网关网络 |
| TCP/IP | Gateway 通信 |
| crypto/hash | OTA 完整性校验及后续签名验证依赖 |
| module support / modversions | 当前外部驱动模块构建与加载 |

根文件系统启动路径依赖的 MMC 和 ext4 驱动应以内建方式提供，除非明确设计 initramfs 并验证模块加载路径。不能直接将根设备驱动裁剪为模块。

## 4. 裁剪方法

1. 复制原始 .config。
2. 每次只修改一个配置主题。
3. 使用 scripts/config 或 menuconfig 修改。
4. 执行 olddefconfig。
5. 保存完整 diff。
6. 编译 Kernel、DTB 和模块。
7. 使用安全启动介质进行板端验证。
8. 记录镜像大小、启动日志和功能回归。

优先评估不使用的 SoC/板级平台支持、不使用的文件系统、不使用的网络协议、不使用的外设驱动和不必要的调试选项。

不允许仅凭“看起来没用”关闭驱动或子系统。需结合 DTS、实际设备、模块依赖、启动日志和业务测试确认。

## 5. DTS 与 CMA 审计

板端本轮实测：
- CmaTotal = 327,680 KiB（320 MiB）
- CmaFree = 226,248 KiB（约 220.9 MiB）

审计内容：
- DTS 中 reserved-memory / linux,cma 节点；
- Kernel CONFIG_CMA / CONFIG_CMA_SIZE_MBYTES 等配置；
- 启动参数是否覆盖 CMA 配额；
- LCD、显示、DMA、摄像头或其他设备的连续内存需求；
- 运行时 CMA 分配和失败日志。

只有证明当前配额大于设备需求并完成压力测试后，才提出缩减值。CmaFree 单次观测不能单独作为缩减依据。

## 6. Buildroot RootFS 定制

- 复现现有 Buildroot 2020.02 配置和工具链。
- 保存原始 .config、BusyBox 配置和 overlay。
- 分析 Gateway、Supervisor、OTA Agent、Sensor Agent 的动态库和命令依赖。
- 精简不使用的软件包和 BusyBox applet。
- 区分调试镜像与生产镜像，生产镜像保留必要故障诊断能力。
- 保留 init/inittab、网络配置、ADB、日志和设备节点管理所需组件。

## 7. 构建和回归指标

裁剪前后记录：
- zImage 大小；
- DTB 大小；
- modules 总大小；
- RootFS 镜像大小及实际文件占用；
- Kernel 启动耗时；
- MemAvailable；
- CmaTotal/CmaFree；
- GPIO、I2C、WiFi、ADB、Gateway、Supervisor 和应用 OTA 回归结果。

目标值应在获得原始构建产物后确定。镜像大小减少 10%/15% 可作为探索目标，不是强制验收条件；不得为达到百分比而破坏功能或安全性。
