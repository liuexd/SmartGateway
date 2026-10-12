#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/platform_device.h>
#include <linux/of.h>

/*
 * 设备树匹配表
 */
static const struct of_device_id demo_of_match[] = {
    {
        .compatible = "smartgateway,bsp-demo",
    },
    { }
};

//作用生成模块设备别名信息
//核心作用：支持内核模块的自动加载（热插拔机制）
MODULE_DEVICE_TABLE(of, demo_of_match);

/*
 * 当设备与驱动匹配时输出日志
 */
static int demo_probe(struct platform_device *pdev)
{
    dev_info(&pdev->dev,
             "probe: device matched\n");

    return 0;
}

/*
 * 当设备移除时输出日志
 */
static int demo_remove(struct platform_device *pdev)
{
    dev_info(&pdev->dev,
             "remove: device removed\n");

    return 0;
}

static struct platform_driver demo_driver = {
    .probe = demo_probe,
    .remove = demo_remove,

    .driver = {
        .name = "platform_dt_demo",

        //匹配设备树兼容性
        .of_match_table = demo_of_match,
    },
};

//模块初始化
static int __init demo_init(void)
{
    int ret;

    ret = platform_driver_register(&demo_driver);

    if(ret)
    {
        pr_err("platform_dt_demo: driver register failed\n");
        return ret;
    }

    pr_info("platform_dt_demo: driver loaded\n");

    return 0;
}

static void __exit demo_exit(void)
{
    platform_driver_unregister(&demo_driver);

    pr_info("platform_dt_demo: driver unloaded\n");
}

module_init(demo_init);
module_exit(demo_exit);


MODULE_LICENSE("GPL");
MODULE_AUTHOR("SmartGateway-BSP");
MODULE_DESCRIPTION("Platform drive with Device Tree matching demo");