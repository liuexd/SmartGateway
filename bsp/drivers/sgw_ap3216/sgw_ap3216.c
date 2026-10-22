
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/i2c.h>
#include <linux/errno.h>
#include <linux/slab.h>
#include <linux/mutex.h>
#include <linux/device.h>
#include <linux/sysfs.h>
#include <linux/of.h>

#define SGW_AP3216_REG_SYS_CONF  0x00
//环境光传感器的Low位
#define SGW_AP3216_REG_ALS_LOW   0x0C
//环境光传感器的High位
#define SGW_AP3216_REG_ALS_HIGH  0x0D

#define SGW_AP3216_MODE_ALL 0x03

/* D12: controlled probe failure injection */
static bool inject_probe_failure;

module_param(inject_probe_failure, bool, 0444);

MODULE_PARM_DESC(
    inject_probe_failure,
    "Inject a failure after AP3216C mode initialization"
);

/* D13: ALS read fault injection */
static int inject_als_failure_stage;

module_param(inject_als_failure_stage, int, 0444);

MODULE_PARM_DESC(
    inject_als_failure_stage,
    "0=off, 1=low byte -EIO, 2=high byte -ETIMEDOUT"
);

struct sgw_ap3216_data {
    struct i2c_client *client;
    struct mutex lock;

    u8 old_mode;
    bool mode_changed;
};

/*
 * 用户读取 als_raw 时，由 sysfs 框架调用。
 * Linux 4.9 中 show() 可以使用 scnprintf() 输出数据。
 */
static ssize_t als_raw_show(
    struct device *dev,
    struct device_attribute *attr,
    char *buf)
{
    struct sgw_ap3216_data *data;
    int low;
    int high;
    int raw;

    (void)attr;

    data = dev_get_drvdata(dev);
    if (!data)
        return -ENODEV;

    mutex_lock(&data->lock);

    if (inject_als_failure_stage == 1) {
        low = -EIO;
    } else {
        low = i2c_smbus_read_byte_data(
            data->client,
            SGW_AP3216_REG_ALS_LOW
        );
    }

    if (low < 0) {
        raw = low;
        goto out_unlock;
    }

    if (inject_als_failure_stage == 2) {
        high = -ETIMEDOUT;
    } else {
        high = i2c_smbus_read_byte_data(
            data->client,
            SGW_AP3216_REG_ALS_HIGH
        );
    }

    if (high < 0) {
        raw = high;
        goto out_unlock;
    }

    raw = (high << 8) | low;

    mutex_unlock(&data->lock);

    return scnprintf(buf, PAGE_SIZE, "%d\n", raw);

out_unlock:
    mutex_unlock(&data->lock);
    return raw;
}

/* 创建只读 sysfs 属性 als_raw */
static DEVICE_ATTR_RO(als_raw);

static struct attribute *sgw_ap3216_attrs[] = {
    &dev_attr_als_raw.attr,
    NULL,
};

static const struct attribute_group sgw_ap3216_attr_group = {
    .attrs = sgw_ap3216_attrs,
};


static int sgw_ap3216_probe(
    struct i2c_client *client,
    const struct i2c_device_id *id)
{
    struct sgw_ap3216_data *data;
    int mode;
    int verify;
    int ret;

    (void)id;

    if (!i2c_check_functionality(
            client->adapter,
            I2C_FUNC_SMBUS_BYTE_DATA))
        return -EOPNOTSUPP;

    data = kzalloc(sizeof(*data), GFP_KERNEL);
    if (!data)
        return -ENOMEM;

    data->client = client;
    mutex_init(&data->lock);

    i2c_set_clientdata(client, data);

    /* 1. 保存原有工作模式 */
    mode = i2c_smbus_read_byte_data(
        client, SGW_AP3216_REG_SYS_CONF);

    if (mode < 0) {
        ret = mode;
        goto err_free_data;
    }

    data->old_mode = (u8)mode;

    /* 2. 如果尚未启用目标模式，则主动配置 */
    if (mode != SGW_AP3216_MODE_ALL) {
        /*
         * 写入可能部分成功，因此在尝试写入前
         * 就标记需要进行恢复处理。
         */
        data->mode_changed = true;

        ret = i2c_smbus_write_byte_data(
            client,
            SGW_AP3216_REG_SYS_CONF,
            SGW_AP3216_MODE_ALL);

        if (ret < 0)
            goto err_restore_mode;

        /* 3. 读回验证 */
        verify = i2c_smbus_read_byte_data(
            client, SGW_AP3216_REG_SYS_CONF);

        if (verify < 0) {
            ret = verify;
            goto err_restore_mode;
        }

        if (verify != SGW_AP3216_MODE_ALL) {
            ret = -EIO;
            goto err_restore_mode;
        }
    }

    /* D12: simulate a failure before sysfs registration */
    if (inject_probe_failure) {
        dev_warn(&client->dev,
                "fault injection: old=0x%02x, changed=%d\n",
                data->old_mode,
                data->mode_changed);

        ret = -EIO;
        goto err_restore_mode;
    }


    /* 4. 创建用户态读取接口 */
    ret = sysfs_create_group(
        &client->dev.kobj,
        &sgw_ap3216_attr_group);

    if (ret < 0)
        goto err_restore_mode;

    dev_info(&client->dev,
             "probe success: old=0x%02x current=0x%02x\n",
             data->old_mode,
             SGW_AP3216_MODE_ALL);

    return 0;

err_restore_mode:
    if (data->mode_changed) {
        int restore_ret;

        restore_ret = i2c_smbus_write_byte_data(
            client,
            SGW_AP3216_REG_SYS_CONF,
            data->old_mode);

        if (restore_ret < 0)
            dev_warn(&client->dev,
                     "restore failed: %d\n",
                     restore_ret);
    }

err_free_data:
    i2c_set_clientdata(client, NULL);
    kfree(data);
    return ret;
}



static int sgw_ap3216_remove(
    struct i2c_client *client)
{
    struct sgw_ap3216_data *data;
    int ret;

    data = i2c_get_clientdata(client);

    /* 先移除用户态访问入口 */
    sysfs_remove_group(
        &client->dev.kobj,
        &sgw_ap3216_attr_group);

    /* 如果驱动修改过模式，则恢复原状态 */
    if (data->mode_changed) {
        ret = i2c_smbus_write_byte_data(
            client,
            SGW_AP3216_REG_SYS_CONF,
            data->old_mode);

        if (ret < 0)
            dev_warn(&client->dev,
                     "remove: restore failed: %d\n",
                     ret);
    }

    i2c_set_clientdata(client, NULL);
    kfree(data);

    dev_info(&client->dev, "remove completed\n");

    return 0;
}


static const struct i2c_device_id sgw_ap3216_id[] = {
    { "sgw_ap3216", 0 },
    { }
};

static const struct of_device_id sgw_ap3216_of_match[] = {
    { .compatible = "smartgateway,ap3216" },
    { }
};

MODULE_DEVICE_TABLE(of, sgw_ap3216_of_match);

MODULE_DEVICE_TABLE(i2c, sgw_ap3216_id);

static struct i2c_driver sgw_ap3216_driver = {
    .driver = {
        .name = "sgw_ap3216",
        .of_match_table = of_match_ptr(sgw_ap3216_of_match),
    },
    .probe = sgw_ap3216_probe,
    .remove = sgw_ap3216_remove,
    .id_table = sgw_ap3216_id,
};

module_i2c_driver(sgw_ap3216_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("SmartGateway-BSP");
MODULE_DESCRIPTION("SmartGateway AP3216C ALS sysfs driver");
