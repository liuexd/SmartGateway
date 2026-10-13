#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/err.h>
#include <linux/string.h>
#include <linux/errno.h>

#define DEMO_NAME "smartgateway-devm-demo"

/*
 * Whether to simulate a probe failure
 *
 * false: normal probe
 * ture: return -EIO after memory allocation
 */
static bool fail_probe =false;

module_param(fail_probe, bool, 0444);

MODULE_PARM_DESC(
    fail_probe,
    "Simulate probe failure after devm_kzalloc"
);

/*
 * Device private data
 */
struct devm_demo_data{
    unsigned int counter;
    char message[64];
};

/*
 * Called after device-driver matching
 */
static int demo_probe(struct platform_device *pdev)
{
    struct devm_demo_data *data;

    dev_info(&pdev->dev, "probe: called\n");

    /*
     * Allocate zero-initialized, device-managed memory
     */

    data = devm_kzalloc(
        &pdev->dev,

        sizeof(*data),
        GFP_KERNEL
    );

    if (data == NULL) {
        dev_err(&pdev->dev, "memory allocation failed\n");
        return -ENOMEM;
    }

    /*
     * kzalloc initializes memory to zero
     */
    dev_info(&pdev->dev,"initial counter = %u\n",
        data->counter);

    data->counter = 100;

    strlcpy(
        data->message,
        "Hello from devm_kzalloc",
        sizeof(data->message)
    );

    /*
     * Simulate an initialization failure
     *
     * devm_kzalloc() has already succeeded
     */
    if(fail_probe)
    {
        dev_warn(&pdev->dev,
            "probe: simulated failure, returning -EIO\n");

            return -EIO;
    }
    /*
     * Save private data only after initialization succeeds
     */
    platform_set_drvdata(pdev,data);

    dev_info(&pdev->dev,
        "probe: counter = %u, message = %s\n",
        data->counter,
        data->message);

    return 0;
}

/*
 * Called when the driver is unbound
 */
static int demo_remove(struct platform_device *pdev)
{
    struct devm_demo_data *data;

    data = platform_get_drvdata(pdev);

    if(data != NULL)
    {
        dev_info(&pdev->dev,
            "remove: counter = %u, message =%s\n",
            data->counter,
            data->message
        );
    }
    /*
    * Do not call kfree(data)
    * Device-managed memory will be released by
    * the driver core after device unbinding
    */
   return 0;
}

static struct platform_driver demo_driver= {
    .probe = demo_probe,
    .remove = demo_remove,

    .driver = {
        .name = DEMO_NAME,
    },
};

static struct platform_device *demo_device;

/*
 *Module initialization
 */
static int __init devm_demo_init(void)
{
    int ret;

    pr_info("devm_demo: module init\n");

    ret = platform_driver_register(&demo_driver);

    if (ret) {
        pr_err("devm_demo: driver registration failed\n");
        return ret;
    }

    /*
     * Create a software Platform Device
     */
    demo_device = platform_device_register_simple(
        DEMO_NAME,
        PLATFORM_DEVID_NONE,
        NULL,
        0
    );

    if(IS_ERR(demo_device)){
        ret = PTR_ERR(demo_device);

        pr_err("devm_demo: device registration failed\n");

        platform_driver_unregister(&demo_driver);

        return ret;
    }

    pr_info("devm_demo: module loaded\n");

    return 0;
}

/*
 * Module cleanup
 */
static void __exit devm_demo_exit(void)
{
    pr_info("devm_demo: module exit begin\n");

    /*
     * Unregister the device first
     * This will trigger demo_remove()
     */
    platform_device_unregister(demo_device);

    /*
     * Unregister the driver
     */
    platform_driver_unregister(&demo_driver);

    pr_info("devm_demo: module unloaded\n");
}

module_init(devm_demo_init);
module_exit(devm_demo_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("SmartGateway-BSP");
MODULE_DESCRIPTION("Device-managed resource lifecycle demo");