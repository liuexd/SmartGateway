
# D04 - Device Tree and Platform Driver

## 1. 实验环境

- Hardware: 100ASK i.MX6ULL
- Architecture: ARMv7
- Kernel: Linux 4.9.88
- Cross Compiler: arm-linux-gnueabihf-gcc
- Development Host: Ubuntu VMware

## 2. D04-1：Device Tree compatible 匹配

实验模块：`bsp/drivers/dt_match_demo/`

使用以下内核接口：

- `of_find_node_by_path()`
- `of_match_node()`
- `of_node_put()`

开发板验证结果：

```text
dt_match_demo: module init
dt_match_demo: matched fsl,imx35-gpio
dt_match_demo: data = IMX35-compatible GPIO controller
```

结果：成功读取现有 GPIO 节点并完成 compatible 匹配。

## 3. D04-2：OF Platform Driver

实验模块：`bsp/drivers/platform_dt_demo/`

实现：

- `platform_driver_register()`
- `platform_driver_unregister()`
- `of_match_table`
- `MODULE_DEVICE_TABLE(of, ...)`
- `probe()` / `remove()` 回调

设备树匹配标识：

`smartgateway,bsp-demo`

编译结果：

- ARM ELF 模块生成成功
- 内核 vermagic 匹配
- modinfo 成功生成 OF alias

开发板验证结果：

```text
platform_dt_demo: driver loaded
platform_dt_demo: driver unloaded
```

结果：Platform Driver 注册、注销成功。

注意：当前运行设备树中没有相应的实验设备节点，因此尚未验证 OF 自动绑定和 probe()。

## 4. D04-3：最小 DTS/DTB 实验

源码：

`bsp/dts/d04-demo.dts`

完成：

DTS → DTB → DTS 反编译

验证自定义 compatible、status 和 demo-message 属性正常保留。

## 5. D04-4：i.MX6ULL 板级设备树实验

源码：

`bsp/dts/d04-imx6ull-smartgateway.dts`

通过包含原始板级 DTS 的方式，新增：

```dts
smartgateway_demo: smartgateway-demo {
    compatible = "smartgateway,bsp-demo";
    status = "okay";
    demo-message = "Hello from I.MX6ULL Device Tree";
};
```

通过 cpp 预处理和 dtc 编译，成功生成完整测试 DTB。

反编译验证确认自定义节点存在。

说明：

- 构建及反编译过程中存在 dtc Warning
- 尚未完成与原厂设备树的警告对比
- 未将测试 DTB 写入开发板
- 未修改 eMMC 启动文件

## 6. 当前结论

已掌握：

- DTS / DTSI / DTB 的基本关系
- compatible 匹配原理
- of_device_id 和 MODULE_DEVICE_TABLE
- Platform Driver 注册与注销
- DTS 预处理、编译和反编译流程

待完成：

- 自定义设备树节点的实机识别
- OF 自动匹配与 probe() 验证
- remove() 与设备解绑验证
- 安全启动及恢复方案验收
