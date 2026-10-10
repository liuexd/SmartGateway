#include "linux/init.h"
#include "linux/module.h"
#include "linux/platform_device.h"
#include "linux/err.h"

//软件模拟的Platdoem Device
static struct platform_device * demo_pdev;

//设备匹配成功后调用
static int demo_probe(struct platform_device *pdev)
{
    dev_info(&pdev->dev, "probe: device matched\n");
    return 0;
}

//解绑时调用
static int demo_remove(struct platform_device *pdev)
{
    dev_info(&pdev->dev, "remove: device unbound\n");
    return 0;
}
//定义Platform Driver
static struct platform_driver demo_driver =
{
    .probe = demo_probe,
    .remove = demo_remove,
    .driver ={
        .name = "bsp_demo_device",
    },
};

//模块加载入口
static int __init demo_init(void)
{
    int ret;

    ret = platform_driver_register(&demo_driver);
    if(ret)
        return ret;

    demo_pdev = platform_device_register_simple(
        "bsp_demo_device",
        PLATFORM_DEVID_NONE,
        NULL,
        0
    );

    if(IS_ERR(demo_pdev))
    {
        ret = PTR_ERR(demo_pdev);
        demo_pdev = NULL;
        platform_driver_unregister(&demo_driver);
        return ret;
    }

    pr_info("platform_demo: module loaded\n");
    return 0;
}

static void __exit demo_exit(void)
{
    platform_device_unregister(demo_pdev);
    platform_driver_unregister(&demo_driver);

    pr_info("platform_demo: module unloaded\n");
}

module_init(demo_init);
module_exit(demo_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("SmartGateway-BSP");
MODULE_DESCRIPTION("Platform bus matching demonstration");
