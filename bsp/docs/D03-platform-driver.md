# D03 · Platform Driver 开发与实机验证

## 1. 开发环境

- 开发板：100ASK i.MX6ULL
- Linux Kernel：4.9.88
- 交叉编译工具链：Linaro GCC 6.2.1
- 开发分支：feature/bsp-imx6ull
- 日期：2026-10-10

## 2. 开发目标

学习 Linux Platform Bus 的设备与驱动匹配机制，实现一个软件模拟的 Platform Device 和 Platform Driver，并验证 probe() 与 remove() 回调函数。

## 3. 实现内容

新增 `bsp/drivers/platform_demo/` 目录：

- `platform_demo.c`：实现软件 Platform Device、Platform Driver、probe()、remove()。
- `Makefile`：通过 Linux 4.9.88 内核构建系统交叉编译。
- `.gitignore`：忽略内核模块编译生成文件。

设备与驱动名称均为 `bsp_demo_device`，通过 Platform Bus 名称匹配机制完成绑定。

本次实验不修改设备树，不访问真实 GPIO 寄存器。

## 4. 开发板验证结果

### 4.1 Platform Device 与 Driver 绑定成功

实际检查：

`/sys/bus/platform/devices/bsp_demo_device/driver`

符号链接指向：

`../../../bus/platform/drivers/bsp_demo_device`

### 4.2 probe() 执行成功

实际内核日志：

```text
[90531.626783] bsp_demo_device bsp_demo_device: probe: device matched
[90531.653550] platform_demo: module loaded
```

### 4.3 remove() 与模块卸载成功

实际内核日志：

```text
[92025.797920] bsp_demo_device bsp_demo_device: remove: device unbound
[92025.815989] platform_demo: module unloaded
```

`rmmod platform_demo` 的退出状态为 0。

卸载后，`/sys/bus/platform/devices/` 中已不再出现 `bsp_demo_device`，原 driver 符号链接也不存在。

## 5. 验收结论

D03 的核心功能验收通过，确认了 Platform Device 注册、Platform Driver 名称匹配、probe() 执行、remove() 执行和设备注销流程。

本阶段尚未验证 Device Tree compatible 匹配机制。

## 6. 后续计划

D04：学习 Device Tree、of_match_table 和 compatible 匹配机制。

D01 原厂恢复方案仍需单独完成验收。