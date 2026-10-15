
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

struct sgw_gpio_event {
    uint64_t timestamp_ns;
    uint32_t sequence;
    uint32_t level;
};

int main(void)
{
    int fd;
    ssize_t ret;
    struct sgw_gpio_event event;

    fd = open("/dev/sgw_event0",
              O_RDONLY | O_NONBLOCK);

    if (fd < 0) {
        perror("open");
        return EXIT_FAILURE;
    }

    errno = 0;
    ret = read(fd, &event, sizeof(event));

    if (ret == -1 && errno == EAGAIN) {
        printf("PASS: empty queue returns EAGAIN\n");
    } else if (ret == sizeof(event)) {
        printf("Queue not empty: seq=%u level=%u\n",
               event.sequence, event.level);
        close(fd);
        return EXIT_FAILURE;
    } else {
        printf("FAIL: read=%zd errno=%d\n",
               ret, errno);
        close(fd);
        return EXIT_FAILURE;
    }

    close(fd);
    return EXIT_SUCCESS;
}
