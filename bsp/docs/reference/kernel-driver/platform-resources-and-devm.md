
# D05 - Platform Resources and Device-Managed Memory

## 1. 实验环境

- Board: 100ASK i.MX6ULL
- Architecture: ARMv7
- Kernel: Linux 4.9.88
- Cross Compiler: arm-linux-gnueabihf-gcc
- Branch: feature/bsp-imx6ull

## 2. 实验目标

掌握以下 Linux 内核驱动接口：

- bus_find_device_by_name()
- to_platform_device()
- platform_get_resource()
- platform_get_irq()
- resource_size()
- devm_kzalloc()
- platform_set_drvdata()
- platform_get_drvdata()

并验证正常解绑和 probe() 失败时的资源生命周期。

## 3. D05-1：设备树资源分析

实验设备：209c000.gpio

当前绑定驱动：gpio-mxc

GPIO1 设备树资源：

    reg = <0x0209c000 0x4000>;

    interrupts = <GIC_SPI 66 IRQ_TYPE_LEVEL_HIGH>,
                 <GIC_SPI 67 IRQ_TYPE_LEVEL_HIGH>;

内存资源：

- Start: 0x0209c000
- End: 0x0209ffff
- Size: 0x4000 (16 KiB)

本实验不修改 GPIO 寄存器，不解绑原厂驱动。

## 4. D05-2：Platform MEM Resource

模块：platform_resource_demo.ko

通过 bus_find_device_by_name() 查找现有设备。

通过 platform_get_resource() 获取内存资源。

实机结果：

    platform_resource_demo: device = 209c000.gpio
    platform_resource_demo: resource = [mem 0x0209c000-0x0209ffff flags 0x200]
    platform_resource_demo: size = 0x4000 bytes

结论：

MEM 资源与设备树 reg 属性一致。

通过 put_device() 释放查找过程中获取的设备引用。

## 5. D05-3：Linux IRQ Resource

调用：

    platform_get_irq(pdev, 0);
    platform_get_irq(pdev, 1);

实机结果：

    Linux IRQ0 = 30
    Linux IRQ1 = 31

设备树中对应的 GIC SPI 编号为 66、67。

GPIO 上级中断使用 Linux IRQ Domain 映射。

在当前内核中，IRQ 30、31 未作为普通处理项显示在
/proc/interrupts 和 /proc/irq 中。

检查 gpio-mxc.c 发现其使用
irq_set_chained_handler() 和
irq_set_chained_handler_and_data() 实现级联中断处理。

本实验未申请或接管真实 IRQ。

## 6. D05-4A：devm_kzalloc 正常路径

模块：devm_demo.ko

通过软件 Platform Device 进行名称匹配。

probe() 中：

- devm_kzalloc() 分配托管内存
- 验证初始 counter = 0
- 将 counter 修改为 100
- platform_set_drvdata() 保存数据

remove() 中：

- platform_get_drvdata() 获取数据
- 验证 counter = 100
- 不调用 kfree()

实机结果：

    probe: initial counter = 0
    probe: counter = 100
    remove: counter = 100

模块正常加载、解绑和卸载。

## 7. D05-4B：probe 失败路径

通过模块参数 fail_probe 控制测试。

### 成功路径

    insmod devm_demo.ko fail_probe=0

结果：

- probe() 成功
- Device 与 Driver 绑定成功
- sysfs driver 链接存在
- 卸载时调用 remove()

### 失败路径

    insmod devm_demo.ko fail_probe=1

结果：

- devm_kzalloc() 分配成功
- probe() 返回 -EIO (-5)
- 模块初始化仍然返回 0
- Platform Device 保持注册状态
- Device 与 Driver 绑定失败
- sysfs driver 链接不存在
- 模块卸载时不调用 remove()

## 8. 技术结论

1. Platform Device 注册成功不等于驱动绑定成功。
2. insmod 的结果取决于模块初始化函数返回值。
3. probe() 失败后，通常不会调用对应设备的 remove()。
4. devm_kzalloc() 的内存在绑定失败时由 devres 机制清理。
5. 正常绑定后，devm 内存通常在设备解绑时自动清理。
6. platform_get_resource() 获取资源描述，不代表申请了硬件资源。
7. platform_get_irq() 获取 IRQ 编号，不等于注册中断处理函数。

## 9. 实验边界

- 没有修改开发板原始设备树和启动 DTB
- 没有覆盖 eMMC 内容
- 没有直接访问 GPIO 寄存器
- 没有申请或接管原厂 GPIO IRQ
- devm 自动清理机制尚未通过内存泄漏检测工具独立测量

## 10. 后续任务

- 完善 D01 安全恢复验证
- 完成自定义 OF Platform Device 的真实 probe() 绑定
- 学习设备树 GPIO 属性解析及 GPIO Descriptor API
- 学习驱动资源申请和释放的正确顺序
