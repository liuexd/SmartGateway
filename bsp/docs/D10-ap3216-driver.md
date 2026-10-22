# D10：AP3216C I2C 驱动与设备树匹配

## 1. 实验环境

- 开发板：100ASK i.MX6ULL 14x14
- Linux 内核：4.9.88
- 驱动：sgw_ap3216
- I2C 从机地址：0x1e
- Linux 运行时总线：i2c-0
- 设备树控制器：i2c@021a0000

## 2. 驱动功能验证

### 2.1 内核模块编译

sgw_ap3216.ko 编译成功，模块 vermagic 与目标内核版本 4.9.88 一致。

### 2.2 I2C 设备创建与驱动匹配

通过 sysfs 的 new_device 接口创建测试设备：

    echo sgw_ap3216 0x1e > \
        /sys/bus/i2c/devices/i2c-0/new_device

内核日志出现 probe success，证明手动创建的 I2C 设备成功匹配驱动并完成初始化。

### 2.3 环境光数据读取

通过 sysfs 属性读取环境光原始数据：

    cat /sys/bus/i2c/devices/0-001e/als_raw

测试过程中观察到原始值随光照变化，例如 80、85、84。

这些值是传感器原始数据，不是经过校准的 lux 照度值。

### 2.4 资源清理与硬件状态恢复

删除测试设备后，内核日志出现 remove completed。

删除设备后读取 AP3216C 系统配置寄存器，结果为 0x00，与 probe 前保存的原始模式一致。

随后卸载 sgw_ap3216 模块，lsmod 不再显示该模块。

## 3. 设备树匹配

驱动 OF 匹配表包含：

    { .compatible = "smartgateway,ap3216" }

设备树节点为：

    ap3216@1e {
        compatible = "smartgateway,ap3216";
        reg = <0x1e>;
        status = "okay";
    };

节点位于 i2c@021a0000 控制器下。

## 4. 离线编译验证

修改目标文件：

    arch/arm/boot/dts/100ask_imx6ull-14x14.dts

编译命令：

    make HOST_EXTRACFLAGS=-fcommon \
        ARCH=arm \
        CROSS_COMPILE=arm-linux-gnueabihf- \
        100ask_imx6ull-14x14.dtb

DTB 编译成功，并通过反编译确认 AP3216C 节点包含正确的 compatible、reg 和 status 属性。

旧版 DTC 在现代主机编译器下出现 yylloc 重复定义问题，增加 HOST_EXTRACFLAGS=-fcommon 后成功构建。

## 5. 当前限制与后续工作

1. 已验证手动创建 I2C 设备时的驱动匹配与数据读取。
2. 已完成设备树节点的离线编译检查。
3. 尚未在开发板上启动加载包含新节点的 DTB，因此设备树自动创建设备和自动 probe 尚未完成上板验证。
4. 开发板运行时的 model 和 compatible 不能唯一确定实际使用的板级 DTS。部署前需要进一步确认 U-Boot 实际加载的 DTB。
5. 后续应在备份原始 DTB、确认启动配置和准备恢复方案后，再进行设备树上板验证。
