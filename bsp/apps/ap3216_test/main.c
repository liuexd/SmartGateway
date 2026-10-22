
#include <stdio.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/i2c.h>
#include <linux/i2c-dev.h>

#define I2C_DEVICE "/dev/i2c-0"
#define AP3216_ADDR 0x1E

static int ap3216_read_reg(int fd, uint8_t reg, uint8_t *value)
{
    struct i2c_msg msgs[2] = {
        {
            .addr = AP3216_ADDR,
            .flags = 0,
            .len = 1,
            .buf = &reg,
        },
        {
            .addr = AP3216_ADDR,
            .flags = I2C_M_RD,
            .len = 1,
            .buf = value,
        }
    };

    struct i2c_rdwr_ioctl_data transfer = {
        .msgs = msgs,
        .nmsgs = 2
    };

    return ioctl(fd, I2C_RDWR, &transfer);
}

int main(void)
{
    int fd;
    uint8_t mode;
    uint8_t low;
    uint8_t high;
    unsigned int als_raw;

    fd = open(I2C_DEVICE, O_RDWR);
    if (fd < 0) {
        perror("open i2c");
        return 1;
    }

    if (ap3216_read_reg(fd, 0x00, &mode) < 0) {
        perror("read mode");
        close(fd);
        return 1;
    }

    printf("AP3216 mode = 0x%02X\n", mode);

    if (mode != 0x03) {
        fprintf(stderr, "Sensor is not in ALS+PS+IR mode\n");
        close(fd);
        return 1;
    }

    if (ap3216_read_reg(fd, 0x0C, &low) < 0 ||
        ap3216_read_reg(fd, 0x0D, &high) < 0) {
        perror("read ALS");
        close(fd);
        return 1;
    }

    als_raw = ((unsigned int)high << 8) | low;

    printf("ALS LOW  = 0x%02X\n", low);
    printf("ALS HIGH = 0x%02X\n", high);
    printf("ALS RAW  = %u\n", als_raw);

    close(fd);
    return 0;
}
