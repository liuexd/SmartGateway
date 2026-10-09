/*模块*/
#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>

/* 模块加载时调用 */
static int __init bsp_hello_init(void)
{
    pr_info("bsp_hello: module initialized\n");

    return 0;
}

/* 模块卸载时调用 */
static void __exit bsp_hello_exit(void)
{
    pr_info("bsp_hello: module exited\n");
}

module_init(bsp_hello_init);
module_exit(bsp_hello_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("SmartGateway-BSP");
MODULE_DESCRIPTION("First i.MX6ULL Linux kernel module");
