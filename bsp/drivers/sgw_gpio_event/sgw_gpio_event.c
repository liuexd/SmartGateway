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

#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/poll.h>
#include <linux/wait.h>
#include <linux/mutex.h>

#include <linux/workqueue.h>
#include <linux/jiffies.h>

#define SGW_NAME "sgw-gpio-event"
#define SGW_GPIO 115
#define SGW_EVENT_CAPACITY 64
#define SGW_DEBOUNCE_MS 20

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

    struct miscdevice miscdev;
    wait_queue_head_t read_queue;
    struct mutex read_lock;

    struct delayed_work debounce_work;
    int stable_level;
};


/*
 * Runs in workqueue context, not hard IRQ context.
 * Confirm GPIO state after a 20 ms quiet period.
 */
static void sgw_gpio_debounce_work(struct work_struct *work)
{
    struct sgw_gpio_dev *data =
        container_of(
            to_delayed_work(work),
            struct sgw_gpio_dev,
            debounce_work);

    struct sgw_gpio_event event;
    unsigned long flags;
    bool queued = false;
    int level;

    level = gpio_get_value(data->gpio);

    event.timestamp_ns = ktime_get_ns();
    event.level = level;

    spin_lock_irqsave(&data->queue_lock, flags);

    /*
     * No actual stable state transition:
     * do not generate a duplicate event.
     */
    if (level != data->stable_level) {

        data->stable_level = level;

        event.sequence =
            data->enqueued + data->dropped + 1;

        data->last_event = event;

        if (data->count < SGW_EVENT_CAPACITY) {

            data->events[data->head] = event;

            data->head =
                (data->head + 1) % SGW_EVENT_CAPACITY;

            data->count++;
            data->enqueued++;

            queued = true;

        } else {
            data->dropped++;
        }
    }

    spin_unlock_irqrestore(&data->queue_lock, flags);

    if (queued)
        wake_up_interruptible(&data->read_queue);
}


/*
 * IRQ Handler:
 * Capture the event and insert it into the ring buffer
 *
 * The handler must no sleep
 */

static irqreturn_t sgw_gpio_irq_handler(
    int irq, void *dev_id)
{
    struct sgw_gpio_dev *data = dev_id;
    unsigned long flags;

    (void)irq;

    /* Count every raw hardware interrupt. */
    spin_lock_irqsave(&data->queue_lock, flags);

    data->irq_total++;

    spin_unlock_irqrestore(&data->queue_lock, flags);

    /*
     * Restart the debounce interval on each edge.
     * Do not sleep inside IRQ Handler.
     */
    mod_delayed_work(
        system_wq,
        &data->debounce_work,
        msecs_to_jiffies(SGW_DEBOUNCE_MS));

    return IRQ_HANDLED;
}


static int sgw_event_open(
    struct inode *inode,
    struct file *file
)
{
    struct miscdevice *misc = file->private_data;

    struct sgw_gpio_dev *data = container_of(misc, struct sgw_gpio_dev,miscdev);//成员指针，结构体，成员名字

    file->private_data = data;
    //成功返回0，告诉内核这个设备文件不支持lseek()
    //由于设备/dev/sgw-gpio-event是一个事件流（环形缓冲区），数据是GPIO中断时产生的，只能一个按照顺序去读
    return nonseekable_open(inode, file);
}

static ssize_t sgw_event_read(
    struct file *file,
    char __user *buf,
    size_t len,
    loff_t *ppos
)
{
    struct sgw_gpio_dev *data = file->private_data;
    struct sgw_gpio_event event;
    unsigned long flags;
    int ret;

    (void)ppos;

    if(len < sizeof(event))
        return -EINVAL;

    for(;;)
    {
        ret = mutex_lock_interruptible(&data->read_lock);
        if(ret)
            return ret;

        spin_lock_irqsave(&data->queue_lock,flags);
        if(data->count > 0)
        {
            event = data->events[data->tail];

            spin_unlock_irqrestore(&data->queue_lock,flags);
            break;
        }
        spin_unlock_irqrestore(&data->queue_lock,flags);
        mutex_unlock(&data->read_lock);
        
        if(file->f_flags & O_NONBLOCK)
            return -EAGAIN;
        //第一个参数是等待队列头，第二个是唤醒条件
        ret = wait_event_interruptible(
            data->read_queue,
            READ_ONCE(data->count)>0
        );
        if (ret)
            return ret;
    }

    /*
     * Do not hold a spinlock across copy_to_user()
     * Keep read_lock held to serialize comsumer
     */
    if(copy_to_user(buf, &event, sizeof(event)))
    {
        mutex_unlock(&data->read_lock);
        return -EFAULT;
    }
    spin_lock_irqsave(&data->queue_lock, flags);

    data->tail = (data->tail+1)%SGW_EVENT_CAPACITY;
    data->count--;

    spin_unlock_irqrestore(&data->queue_lock,flags);
    mutex_unlock(&data->read_lock);

    return sizeof(event);
}

//wait: 内核提供的"等待队列表",(poll_table),用于挂载等待队列
static unsigned int sgw_event_poll(
    struct file *file,
    poll_table *wait
)
{
    struct sgw_gpio_dev *data = file->private_data;
    unsigned long flags;
    unsigned int mask = 0;

    poll_wait(file, &data->read_queue, wait);

    //这里使用自旋锁的原因是
    //此时如果出现中断会导致这里的count出现变化
    //所以这里选择先锁住在读取内容
    spin_lock_irqsave(&data->queue_lock, flags);

    if (data->count > 0)
        mask = POLLIN | POLLRDNORM;
    
    spin_unlock_irqrestore(&data->queue_lock, flags);

    return mask;
}

static const struct file_operations sgw_event_fops = {
    .owner = THIS_MODULE,
    .open = sgw_event_open,
    .read = sgw_event_read,
    .poll = sgw_event_poll,
    .llseek = no_llseek,
};

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
    init_waitqueue_head(&data->read_queue);
    mutex_init(&data->read_lock);
    INIT_DELAYED_WORK(
        &data->debounce_work,
        sgw_gpio_debounce_work);

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

    data->stable_level = gpio_get_value(data->gpio);

    dev_info(&pdev->dev,
         "initial stable level=%d debounce=%dms\n",
         data->stable_level,
         SGW_DEBOUNCE_MS);

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
        devm_free_irq(&pdev->dev, data->irq, data);
        cancel_delayed_work_sync(&data->debounce_work);
        return ret;
    }

    data->miscdev.minor = MISC_DYNAMIC_MINOR;
    data->miscdev.name = "sgw_event0";
    data->miscdev.fops = &sgw_event_fops;
    data->miscdev.parent = &pdev->dev;

    ret = misc_register(&data->miscdev);

    if (ret) {
        dev_err(&pdev->dev,
                "misc_register failed: %d\n", ret);

        device_remove_file(&pdev->dev, &dev_attr_stats);
        devm_free_irq(&pdev->dev, data->irq, data);
        cancel_delayed_work_sync(&data->debounce_work);
        return ret;
    }

    dev_info(&pdev->dev,
         "misc device /dev/sgw_event0 ready\n");

    
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

    misc_deregister(&data->miscdev);

    device_remove_file(&pdev->dev, &dev_attr_stats);

    /* Stop new IRQ callbacks first. */
    devm_free_irq(&pdev->dev, data->irq, data);

    /* Wait for pending/running debounce work. */
    cancel_delayed_work_sync(&data->debounce_work);

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