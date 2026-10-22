# D14：AP3216C 读取统计与限频错误日志

## 1. 实验目标

在已有 AP3216C I2C 驱动基础上实现：

1. ALS 完整读取成功次数统计。
2. ALS 完整读取失败次数统计。
3. 通过 sysfs 暴露统计结果。
4. 通过 dev_err_ratelimited() 输出限频错误日志。
5. 使用软件故障注入验证错误统计。
6. 关闭注入后验证恢复正常读取。

## 2. 实验环境

- 开发板：100ASK i.MX6ULL 14x14
- Linux 内核：4.9.88
- I2C 适配器：i2c-0
- 传感器地址：0x1e
- 设备名称：0-001e
- 驱动：sgw_ap3216.ko
- 编译环境：Ubuntu + ARM 交叉编译工具链

## 3. 统计计数器设计

驱动私有结构体增加：

    u64 read_success;
    u64 read_errors;

计数器通过 kzalloc() 初始化为 0。

读取成功：

    data->read_success++;

读取失败：

    data->read_errors++;

统计以一次完整的 ALS 测量为单位。

只有低字节和高字节均读取成功，
才增加 read_success。

低字节或高字节任意一次读取失败，
只增加一次 read_errors。

## 4. 并发保护

计数器的更新位于 data->lock 的保护范围内。

stats_show() 使用同一把 mutex，
获取 read_success 和 read_errors 的一致快照。

在 32 位 ARM 平台上，普通 u64 并发访问
需要特别考虑读写一致性。

## 5. sysfs 统计接口

新增只读属性：

    /sys/bus/i2c/devices/0-001e/stats

通过 DEVICE_ATTR_RO(stats) 定义。

属性注册到原有的 sgw_ap3216_attr_group 中。

输出格式：

    read_success=5
    read_errors=0

读取 stats 本身不触发 ALS I2C 通信，
也不增加成功或失败计数。

## 6. 正常读取测试

D14 驱动加载后初始统计：

    read_success=0
    read_errors=0

连续读取 als_raw 五次，
五次均成功返回原始值 0。

随后统计：

    read_success=5
    read_errors=0

重复读取 stats，统计值保持不变。

结论：成功计数功能通过。

## 7. 错误计数测试

使用软件故障注入：

    inject_als_failure_stage=2

模拟 ALS 高字节读取返回：

    -ETIMEDOUT (-110)

连续读取三次，均出现：

    Connection timed out

统计结果：

    read_success=0
    read_errors=3

设备保持绑定，als_raw 属性仍然存在。

结论：错误计数功能通过。

## 8. 限频日志实现

在低字节和高字节读取失败分支中，
分别使用 dev_err_ratelimited() 输出错误日志。

软件注入错误附带：

    [injected]

以区分模拟返回错误与实际 I2C 接口返回错误。

限频日志不会跳过后续错误处理，
也不会影响 read_errors 计数。

## 9. 30 次故障注入测试

加载带限频日志功能的 D14 模块：

    insmod /tmp/sgw_ap3216-d14-log.ko \
        inject_probe_failure=0 \
        inject_als_failure_stage=2

初始统计：

    read_success=0
    read_errors=0

执行：

    for i in $(seq 1 30); do
        cat /sys/bus/i2c/devices/0-001e/als_raw \
            >/dev/null 2>/dev/null
    done

最终统计：

    read_success=0
    read_errors=30

检查 dmesg，观察到 10 条匹配的
ALS high byte read failed 日志。

日志内容：

    ALS high byte read failed: -110 [injected]

结果符合限频日志的预期：
错误计数记录了全部 30 次失败，
但实际打印的日志数量较少。

## 10. 关闭故障注入后的恢复测试

重新加载：

    insmod /tmp/sgw_ap3216-d14-log.ko \
        inject_probe_failure=0 \
        inject_als_failure_stage=0

读取 als_raw：

    0

再次读取 stats：

    read_success=1
    read_errors=0

结果证明正常读取恢复，
且新的驱动实例重新初始化统计计数器。

## 11. 实验结论

D14 核心功能通过验证：

- ALS 成功和失败次数统计正确。
- stats 可以读取一致的计数快照。
- 读取 stats 本身不改变计数器。
- 限频日志没有影响错误计数。
- 故障注入后可以恢复正常采集。

## 12. 当前实现边界

1. 只统计 ALS 测量成功或失败，
   不统计 probe() 中的 I2C 初始化错误。
2. 计数器随驱动解绑和重新绑定而重置。
3. 未实现低字节、高字节错误分类计数。
4. 未验证真实 I2C 总线硬件故障。
5. 未进行 KASAN、lockdep 或长期稳定性测试。
6. 软件故障注入参数主要用于开发调试。

## 13. BSP 面试复盘

Q1：为什么统计计数器使用 mutex？

避免并发更新丢失以及 u64 读取不一致。

Q2：为什么 stats_show() 不更新 read_success？

因为 read_success 统计的是完整 ALS 测量成功次数，
而非普通 sysfs 属性读取次数。

Q3：日志只有 10 条，错误计数为什么是 30？

dev_err_ratelimited() 只限制日志输出，
不限制实际错误处理和计数。

Q4：为什么重新绑定后计数器清零？

因为旧的驱动私有数据已经被释放，
新的 probe() 通过 kzalloc() 分配了新结构体。
