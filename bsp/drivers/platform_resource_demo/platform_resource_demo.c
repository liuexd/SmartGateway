#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/device.h>
#include <linux/platform_device.h>
#include <linux/ioport.h>
#include <linux/errno.h>

#define DEMO_DEVICE_NAME "209c000.gpio"

static int __init resource_demo_init(void)
{
    struct device *dev;
    struct platform_device *pdev;
    struct resource *res;

    int irq0;
    int irq1;

    pr_info("platform_resource_demo: module init\n");

    /*
     * Find  the existing Platform Device by name
     *
     * This function obtains a device reference
     *
     */

     dev = bus_find_device_by_name(
        &platform_bus_type,
        NULL,
        DEMO_DEVICE_NAME
    );

    /*
     * Convert struct device to struct platform_device
     */
    pdev = to_platform_device(dev);

    /*
     * Get the first memory resource
     * No register mapping or hardware access
     * Para:
     * pdev: 要查询的platform device
     * IORESOURCE: 查询内存地址资源
     * 0: 查询第一个该类型资源
     */
    res = platform_get_resource(
        pdev,
        IORESOURCE_MEM,
        0
    );

    if(res == NULL)
    {
        pr_err("platform_resource_demo: MEM resource not found\n");

        put_device(dev);

        return -ENODEV;
    }

    /*
     * Print the resource range
     */
    pr_info("platform_resource_demo: device = %s\n",dev_name(dev));
    //%pr是专门为struct resource结构体设计的一个快捷打印的格式符号
    pr_info("platform_resource_demo: resource = %pr\n", res);
    //%llx, ll:long long, x:代表十六进制
    pr_info("platform_resource_demo: size = 0x%llx bytes\n",(unsigned long long)resource_size(res));

    /*
     * Query Linux IRQ numbers from the Platform Device
     *
     * This does not call request_irq() and does not
     * register an interrupt handler
     */
    irq0 = platform_get_irq(pdev, 0);

    if(irq0 < 0 )
    {
        pr_err("platform_resourece_demo: IRQ0 lookup failed: %d\n",
            irq0);

        put_device(dev);
        return irq0;
    }

    irq1 = platform_get_irq(pdev, 1);

    if(irq1 < 0 )
    {
        pr_err("platform_resourece_demo: IRQ1 lookup failed: %d\n",
            irq1);

        put_device(dev);
        return irq1;
    }
    //设备树当中的硬件中断编号会与Linux IRQ编号不一样
    //在ARM GIC(generic Interrupt Controller通用中断控制器)中
    //设备树的SPI(Shared Peripheral Interrupt 共享外部中断)编号通常需要加上32， 才是GIC中断ID
    //Linux 内核里的编号：Linux 内核给所有的中断分配了一个全局的“软件中断号（Linux IRQ Number）”。
    //在这个全局空间中，0-31 已经被 SGI 和 PPI 占用了。
    pr_info("platform_resource_demo: Linux IRQ0 = %d\n", irq0);
    pr_info("platform_resource_demo: Linux IRQ1 = %d\n", irq1);
    /*
     * Release the reference obtained by
     * bus_find_device_by_name()
     */
    put_device(dev);

    return 0;
}

static void __exit resource_demo_exit(void)
{
    pr_info("platform_resource_demo: module exit\n");
}


module_init(resource_demo_init);
module_exit(resource_demo_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Smartgateway-BSP");
MODULE_DESCRIPTION("Read-only Platform Device resource demo");