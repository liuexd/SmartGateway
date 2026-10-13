#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/platform_device.h>
#include <linux/gpio.h>
#include <linux/interrupt.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/timekeeping.h>
#include <linux/device.h>
#include <linux/err.h>

#define SGW_NAME "sgw-gpio-event"
#define SGW_GPIO 115
#define SGW_EVENT_CAPACITY 64

struct sgw_gpio_event{
    u64 timestamp_ns;
    u32 sequence;
    u32 level;
};

struct sgw_gpio_dev{
    int gpio;
    int irq;

    spinlock_t queue_lock;

    struct sgw_gpio_event events[SGW_EVENT_CAPACITY];

    u32 head;
    u32 tail;
    u32 count;

    u32 irq_total;
    u32 enqueued;
    u32 dropped;

    struct sgw_gpio_event last_event;
};

/*
 * IRQ Handler:
 * Capture the event and insert it into the ring buffer
 * 
 * The handler must no sleep
 */
static irqreturn_t sgw_gpio_irq_handler(
    int irq, void *dev_id
)
{
    struct sgw_gpio_dev *data = dev_id;
    struct sgw_gpio_event event;
    unsigned long flags;

    event.timestamp_ns = ktime_get_ns();
    event.level = gpio_get_value(data->gpio);

    spin_lock_irqsave(&data->queue_lock,flags);

    data->irq_total++;

    event.sequence = data->irq_total;
    data->last_event =event;

    if(data->count < SGW_EVENT_CAPACITY){
        data->events[data->head] = event;

        data->head = (data->head + 1) % SGW_EVENT_CAPACITY;

        data->count++;
        data->enqueued++;
    }else{
        data->dropped++;
    }

    spin_unlock_irqrestore(&data->queue_lock, flags);

    return IRQ_HANDLED;
}

/*
 * Read only diagnostic statistics
 * The snapshot is protected by the same spinlock
 */
static ssize_t stats_show(
    struct device* dev,
    struct device_attribute *attr,
    char *buf
)
{
    struct sgw_gpio_dev *data = dev_get_drvdata(dev);
    struct sgw_gpio_event last;
    unsigned long flags;
    u32 irq_total;
    u32 enqueued;
    u32 dropped;
    u32 count;
    u32 head;
    u32 tail;

    (void)attr;

     spin_lock_irqsave(&data->queue_lock, flags);

    irq_total = data->irq_total;
    enqueued = data->enqueued;
    dropped = data->dropped;
    count = data->count;
    head = data->head;
    tail = data->tail;
    last = data->last_event;

    spin_unlock_irqrestore(&data->queue_lock, flags);

    return scnprintf(
        buf, PAGE_SIZE,
        "irq_total=%u\n"
        "enqueued=%u\n"
        "dropped=%u\n"
        "depth=%u\n"
        "capacity=%u\n"
        "head=%u\n"
        "tail=%u\n"
        "last_sequence=%u\n"
        "last_level=%u\n"
        "last_timestamp_ns=%llu\n",
        irq_total,
        enqueued,
        dropped,
        count,
        SGW_EVENT_CAPACITY,
        head,
        tail,
        last.sequence,
        last.level,
        (unsigned long long)last.timestamp_ns);
}

static DEVICE_ATTR_RO(stats);
/*
 * Phase A:
 * GPIO resource request and input-level read
 * 
 * No IRQ registration or output operation
 */
static int sgw_gpio_probe(struct platform_device *pdev)
{
    struct sgw_gpio_dev *data;
    int ret;

    dev_info(&pdev->dev, "probe: start\n");

    data = devm_kzalloc(
        &pdev->dev, sizeof(*data),GFP_KERNEL
    );
    if(!data)
        return -ENOMEM;

    data->gpio = SGW_GPIO;
    spin_lock_init(&data->queue_lock);

    platform_set_drvdata(pdev, data);
    /*
     * Request GPIO4_IO19 as input
     * Resource is managed by device core
     *
     */
    ret = devm_gpio_request_one(
        &pdev->dev,
        SGW_GPIO,
        GPIOF_IN,
        "sgw-gpio-input"
    );

    if (ret < 0) {
        dev_err(&pdev->dev,
                "GPIO %d request failed: %d\n",
                SGW_GPIO, ret);
        return ret;
    }

    /*GPIO 编号转化为linux IRQ编号*/
    data->irq =gpio_to_irq(data->gpio);
    if(data->irq < 0)
    {
        dev_err(&pdev->dev,
            "gpio_to_irq failed: %d\n",
            data->irq);
            return data->irq;
    }
    /*注册上升沿和下降沿中断*/
    ret = devm_request_irq(
        &pdev->dev,
        data->irq,
        sgw_gpio_irq_handler,
        IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING,
        "sgw-gpio-irq",
        data
    );
    if (ret) {
        dev_err(&pdev->dev,
                "request IRQ failed: %d\n", ret);
        return ret;
    }

    ret =device_create_file(
        &pdev->dev, &dev_attr_stats
    );
    if(ret){
        dev_err(&pdev->dev,
            "create stats failed: %d\n", ret);
        return ret;
    }

    
    dev_info(&pdev->dev,
             "IRQ %d ready, queue capacity=%u\n",
             data->irq, SGW_EVENT_CAPACITY);

    return 0;
}

static int sgw_gpio_remove(struct platform_device *pdev)
{
    struct sgw_gpio_dev *data = 
        platform_get_drvdata(pdev);

    unsigned long flags;
    u32 irq_total;
    u32 dropped;

    device_remove_file(&pdev->dev, &dev_attr_stats);

    /*
     * Stop and synchronize the IRQ before taking
     * the final statistics snapshot
     */
    devm_free_irq(&pdev->dev, data->irq, data);

    spin_lock_irqsave(&data->queue_lock, flags);

    irq_total = data->irq_total;
    dropped = data -> dropped;

    spin_unlock_irqrestore(&data->queue_lock, flags);

    dev_info(&pdev->dev, "remove: irq_total=%u dropped=%u\n",
        irq_total, dropped);

    /*
     * devm-managed GPIO will be released
     * during driver unbinding.
     */
    return 0;
}
static struct platform_driver sgw_driver = {
    .probe = sgw_gpio_probe,
    .remove = sgw_gpio_remove,

    .driver = {
        .name = SGW_NAME,
    },
};

static struct platform_device *sgw_device;

static int __init sgw_init(void)
{
    int ret;

    pr_info("sgw_gpio_event: module init\n");

    ret = platform_driver_register(&sgw_driver);

    if (ret)
        return ret;

    sgw_device = platform_device_register_simple(
        SGW_NAME,
        PLATFORM_DEVID_NONE,
        NULL,
        0
    );

    if (IS_ERR(sgw_device)) {
        ret = PTR_ERR(sgw_device);
        platform_driver_unregister(&sgw_driver);
        return ret;
    }

    pr_info("sgw_gpio_event: module loaded\n");

    return 0;
}

static void __exit sgw_exit(void)
{
    platform_device_unregister(sgw_device);
    platform_driver_unregister(&sgw_driver);

    pr_info("sgw_gpio_event: module unloaded\n");
}

module_init(sgw_init);
module_exit(sgw_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("SmartGateway-BSP");
MODULE_DESCRIPTION("SmartGateway GPIO event driver - Phase A");