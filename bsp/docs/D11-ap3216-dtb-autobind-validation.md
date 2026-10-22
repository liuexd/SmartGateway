# D11：AP3216C 设备树自动匹配上板验证

## 1. 实验目标

验证通过设备树自动创建 I2C 设备，
并在加载 sgw_ap3216 驱动时自动匹配和执行 probe。

## 2. 实验环境

- 开发板：100ASK i.MX6ULL 14x14
- Linux：4.9.88
- 总线：i2c-0
- 从机地址：0x1e
- 测试 DTB：sgw-d11-ap3216.dtb
- 驱动：sgw_ap3216.ko

## 3. DTB 备份与部署

原始 DTB：
/boot/100ask_imx6ull-14x14.dtb

Ubuntu 备份：
~/Desktop/D11-dtb-backup/100ask_imx6ull-14x14.dtb

原始文件与备份文件 SHA256 一致。

测试 DTB 上传到：
/boot/sgw-d11-ap3216.dtb

测试 DTB 上传前后 SHA256 一致。

没有覆盖原始 DTB。

## 4. U-Boot 临时启动

在串口 U-Boot 控制台执行：

    mmc dev 1
    mmc rescan
    setenv fdt_file sgw-d11-ap3216.dtb
    run loadfdt
    fdt addr ${fdt_addr}
    fdt print /soc/aips-bus@02100000/i2c@021a0000/ap3216@1e
    run loadimage
    run mmcargs
    bootz ${loadaddr} - ${fdt_addr}

U-Boot 成功读取测试 DTB，并显示 ap3216@1e 节点。

未执行 saveenv，未修改持久化 U-Boot 环境。

## 5. I2C 设备自动创建

Linux 启动后，无需执行 new_device。

验证结果：

    /sys/bus/i2c/devices/0-001e

设备目录存在。

of_node 指向：

    /soc/aips-bus@02100000/i2c@021a0000/ap3216@1e

说明 Linux 根据设备树创建了 I2C 设备对象。

## 6. 驱动自动匹配

执行：

    insmod /tmp/sgw_ap3216.ko

日志：

    sgw_ap3216 0-001e:
    probe success: old=0x00 current=0x03

driver 链接指向：

    /sys/bus/i2c/drivers/sgw_ap3216

说明设备与驱动成功绑定。

注意：本实验验证的是加载模块后的自动匹配，
并未验证开机时自动加载驱动模块。

## 7. ALS 数据采集

接口：

    cat /sys/bus/i2c/devices/0-001e/als_raw

实测原始值包含：

    0
    66

传感器能够响应光照变化。

## 8. 驱动卸载与生命周期

执行：

    rmmod sgw_ap3216

日志：

    sgw_ap3216 0-001e: remove completed

验证结果：

- 0-001e 设备对象仍然存在。
- driver 链接消失。
- als_raw 属性消失。
- remove 正常执行。

## 9. 实验结论

D11 核心功能 PASS。

完成 DTS、DTB、U-Boot、I2C Core、
Driver Core、probe/remove 的完整验证。

设备树自动创建设备与驱动自动匹配均已上板验证。

## 10. 后续改进

- 验证模块自动加载机制。
- 进行异常路径和并发压力测试。
- 根据产品需求完善传感器低功耗管理。
