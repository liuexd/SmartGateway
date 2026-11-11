# D13：AP3216C I2C 读取异常处理与故障注入

## 1. 实验目标

验证 AP3216C 驱动在 ALS 寄存器读取失败时：

1. 能够正确识别 I2C 错误码。
2. 能够释放 mutex。
3. 能够向 sysfs 用户态传递错误码。
4. 不会因单次读取失败而自动解绑设备。
5. 关闭故障注入后能恢复正常读取。

## 2. 实验环境

- 开发板：100ASK i.MX6ULL 14x14
- Linux：4.9.88
- I2C 总线：i2c-0
- 传感器地址：0x1e
- 设备名称：0-001e
- 驱动：sgw_ap3216.ko
- 接口：/sys/bus/i2c/devices/0-001e/als_raw

## 3. ALS 读取流程

als_raw_show() 执行：

1. dev_get_drvdata() 获取设备私有数据。
2. mutex_lock() 获取互斥锁。
3. 读取 ALS 低字节寄存器 0x0C。
4. 读取 ALS 高字节寄存器 0x0D。
5. 组合 raw = (high << 8) | low。
6. 释放 mutex。
7. 通过 sysfs 返回测量值。

若任一寄存器读取失败：

- 保存负错误码。
- 跳转到 out_unlock。
- 释放 mutex。
- 返回原始负错误码。

任何必要字节读取失败，都不返回不完整的 ALS 数据。

## 4. 故障注入设计

新增模块参数：

    static int inject_als_failure_stage;

    module_param(inject_als_failure_stage, int, 0444);

参数定义：

- 0：不注入故障。
- 1：模拟 ALS 低字节读取返回 -EIO。
- 2：低字节正常读取，高字节模拟返回 -ETIMEDOUT。

故障注入只模拟底层读取接口的返回结果，
不代表物理 I2C 总线实际发生错误。

## 5. 模式 1：低字节读取失败

加载：

    insmod /tmp/sgw_ap3216.ko \
        inject_probe_failure=0 \
        inject_als_failure_stage=1

检查：

    cat /sys/bus/i2c/devices/0-001e/als_raw

实测：

    Input/output error

    exit_code=1

验证结果：

- 错误 -EIO 成功传递至用户态。
- 驱动仍然绑定。
- als_raw 属性仍然存在。
- 模块没有自动卸载。

## 6. 模式 2：高字节读取超时

卸载原模块后加载：

    insmod /tmp/sgw_ap3216.ko \
        inject_probe_failure=0 \
        inject_als_failure_stage=2

参数检查返回：

    2

实测：

    Connection timed out

    exit_code=1

验证结果：

- 错误 -ETIMEDOUT 成功传递至用户态。
- 驱动仍然绑定。
- als_raw 属性仍然存在。
- 单次读取失败不会自动触发 remove()。

## 7. 模式 0：恢复正常读取

卸载故障注入模块后执行：

    insmod /tmp/sgw_ap3216.ko \
        inject_probe_failure=0 \
        inject_als_failure_stage=0

参数检查：

    0

内核日志：

    probe success: old=0x00 current=0x03

ALS 读取：

    0

退出码：

    exit_code=0

说明关闭故障注入后，驱动恢复正常读取。
测量值为 0 并不代表读取失败。

## 8. 错误码分析

-EIO：

- 常见数值为 -5。
- 表示输入输出错误。
- 对应用户态 Input/output error。

-ETIMEDOUT：

- 常见数值为 -110。
- 表示操作超时。
- 对应用户态 Connection timed out。

用户态 cat 的退出状态 1 不等于内核错误码 -5 或 -110。

## 9. 并发与资源管理

读取前获取 mutex。

低字节或高字节读取失败时，
都通过 out_unlock 统一解锁。

如果持锁后直接 return，可能导致互斥锁未释放，
后续访问发生长时间阻塞。

## 10. 实验结论

D13 核心实验 PASS。

验证了两种软件模拟的 I2C 读取错误可以
通过 sysfs 正确传递至用户态。

验证了读取失败不等于设备解绑、驱动卸载或
probe 失败。

验证了关闭故障注入后可以恢复正常采集。

尚未测试真实 I2C 总线物理故障、控制器超时、
错误统计或自动恢复策略。

## 11. 后续优化方向

- 增加 I2C 读取失败计数。
- 增加限频错误日志。
- 根据需求设计有限次数重试。
- 研究 I2C 控制器驱动的超时机制。
- 根据硬件特性评估必要的恢复策略。
