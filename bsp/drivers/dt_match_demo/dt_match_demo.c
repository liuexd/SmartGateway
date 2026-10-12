#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/of.h>
#include <linux/errno.h>

//自定义设备树匹配表
static const struct of_device_id demo_of_match[] = {
    {
        .compatible = "fsl,imx35-gpio",
        .data = "IMX35-compatible GPIO controller",
    },
    {  }    
};

static int __init dt_match_demo_init(void)
{
    struct device_node *np;
    const struct of_device_id *match;

    pr_info("dt_match_demo: module init\n");

    //根据设备树完整路径查找GPIO1节点
    np = of_find_node_by_path("/soc/aips-bus@02000000/gpio@0209c000");

    if(!np){
        pr_err("dt_match_demo: node not found\n");
    }

    //根据compatible查找匹配项
    match = of_match_node(demo_of_match, np);
    if (!match) {
        pr_err("dt_match_demo: compatible not matched\n");
        of_node_put(np);
        return -ENODEV;
    }
    pr_info("dt_match_demo: matched %s\n",
            match->compatible);

    pr_info("dt_match_demo: data = %s\n",
            (const char *)match->data);

    /* 释放设备树节点引用 */
    of_node_put(np);

    return 0;
}

static void __exit dt_match_demo_exit(void)
{
    pr_info("dt_match_demo: module exit\n");
}

module_init(dt_match_demo_init);
module_exit(dt_match_demo_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("SmartGateway-BSP");
MODULE_DESCRIPTION("Read-only Device Tree matching demonstration");