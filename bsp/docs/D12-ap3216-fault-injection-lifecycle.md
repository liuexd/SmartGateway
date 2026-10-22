# D12：AP3216C 故障注入与驱动生命周期验证

## 1. 实验目标

基于 D10、D11 完成的 AP3216C I2C 驱动，实现并验证：

1. probe() 初始化失败后的资源回滚。
2. I2C 设备与驱动模块生命周期的区别。
3. sysfs bind/unbind 解绑与重新绑定。
4. sysfs 读取与驱动解绑之间的有限并发测试。

## 2. 实验环境

- 开发板：100ASK i.MX6ULL 14x14
- Linux：4.9.88
- 传感器：AP3216C
- I2C 总线：i2c-0
- I2C 地址：0x1e
- 设备名称：0-001e
- 驱动模块：sgw_ap3216.ko
- 设备来源：D11 测试 DTB 自动枚举

## 3. 故障注入设计

新增模块参数：

    static bool inject_probe_failure;

    module_param(inject_probe_failure, bool, 0444);

该参数默认关闭。

在 probe() 完成硬件模式初始化和读回验证后、
创建 sysfs 属性之前增加故障注入点。

启用时执行：

    ret = -EIO;
    goto err_restore_mode;

这模拟了硬件初始化后、sysfs 注册前发生的错误。

不代表 sysfs_create_group() 本身发生了真实故障。

## 4. probe 错误回滚

错误路径：

    err_restore_mode:
        尝试恢复旧工作模式

    err_free_data:
        i2c_set_clientdata(client, NULL);
        kfree(data);
        return ret;

关键原则：

- 资源按照与申请相反的顺序释放。
- 恢复硬件模式失败时记录警告。
- 保留导致 probe 失败的原始错误码。
- probe 失败不会自动触发 remove()。

## 5. 故障注入实测

加载命令：

    insmod /tmp/sgw_ap3216.ko inject_probe_failure=1

实际日志：

    fault injection: old=0x00, changed=1
    probe of 0-001e failed with error -5

检查结果：

- 驱动模块仍然处于加载状态。
- 0-001e I2C 设备对象仍然存在。
- driver 符号链接不存在。
- als_raw 属性不存在。

说明模块注册成功不等于具体设备绑定成功。

## 6. 故障后的恢复验证

卸载故障注入模块，再正常加载：

    rmmod sgw_ap3216
    insmod /tmp/sgw_ap3216.ko inject_probe_failure=0

实际日志：

    probe success: old=0x00 current=0x03

验证结果：

- 重新读取到原始模式 0x00。
- 设备成功绑定。
- als_raw 属性重新出现。
- ALS 接口可以读取数据。

结果支持先前的硬件模式回滚已成功。

## 7. bind / unbind 验证

解绑：

    echo 0-001e > \
        /sys/bus/i2c/drivers/sgw_ap3216/unbind

解绑后：

- remove() 正常执行。
- 内核模块保留。
- I2C 设备对象保留。
- driver 链接消失。
- als_raw 属性消失。

重新绑定：

    echo 0-001e > \
        /sys/bus/i2c/drivers/sgw_ap3216/bind

重新绑定后：

- probe() 再次执行。
- 私有数据重新分配、初始化。
- driver 链接重新建立。
- als_raw 属性重新创建。

## 8. 并发交错测试

测试内容：

- 后台循环尝试读取 als_raw 100 次。
- 前台连续执行 3 轮 unbind/bind。
- 等待读取进程结束。
- 检查驱动最终状态与内核日志。

实测：

- 3 次 remove completed。
- 3 次 probe success。
- 读取进程结束。
- 未观察到 AP3216C 相关内核崩溃日志。
- 最终 driver 与 als_raw 均恢复。
- ALS 接口能够读取。

由于读取输出和错误被重定向到 /dev/null，
未统计每次读取的成功率。

此次测试属于有限并发交错测试，
不能证明不存在所有竞态条件或内存错误。

## 9. sysfs 生命周期分析

als_raw_show() 使用驱动私有数据 data。

remove() 按以下顺序清理：

1. sysfs_remove_group()
2. 尝试恢复硬件工作模式
3. i2c_set_clientdata(client, NULL)
4. kfree(data)

在 Linux 4.9 的正常 sysfs/kernfs 活动引用机制下，
删除属性会同步等待相关活动回调结束。

因此先移除 sysfs 属性，再释放 data，
可以避免该 sysfs 回调在释放后继续使用 data。

但如果增加 workqueue、IRQ 或内核线程，
需要对这些额外的异步访问路径单独同步。

## 10. 实验结论

D12 核心功能验证通过：

- probe 错误路径按预期执行。
- 故障后可以恢复正常绑定。
- bind/unbind 生命周期验证通过。
- 有限并发交错测试未发现明显异常。

尚未进行 KASAN、lockdep 或内存泄漏专项检测。

## 11. BSP 面试复盘

Q1：insmod 成功是否等于 probe 成功？

不等于。模块加载与具体设备绑定具有不同生命周期。

Q2：probe 失败之后是否自动执行 remove？

不会。失败路径必须自行清理已获取资源。

Q3：为什么 unbind 之后模块仍然存在？

unbind 只解除设备和驱动的绑定，不卸载模块。

Q4：重新 bind 能否复用旧 data？

不能。旧 data 已在 remove() 中释放，
新的 probe() 必须重新申请和初始化私有数据。

Q5：sysfs_remove_group() 是否可以替代所有并发同步？

不能。它保护的是相关 sysfs 回调的生命周期，
不能自动同步 IRQ、工作队列或其他异步执行路径。
