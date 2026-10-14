#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>

#define SGW_DEVICE "/dev/sgw_event0"

/*
 * Must match the kernel event structure.
 * Layout: 8 + 4 + 4 = 16 bytes.
 */

 struct sgw_gpio_event{
    uint64_t timestamp_ns;
    uint32_t sequence;
    uint32_t level;
};

int main(void)
{
    int fd;
    int ret;
    ssize_t n;

    struct pollfd pfd;
    struct sgw_gpio_event event;

    if(sizeof(event) != 16){
        fprintf(stderr,"Unexpected event size\n");
        return EXIT_FAILURE;
    }

    fd = open(SGW_DEVICE, O_RDONLY | O_NONBLOCK);
    if(fd < 0)
    {
        perror("open");
        return EXIT_FAILURE;
    }
    pfd.fd = fd;
    pfd.events = POLLIN;
    pfd.revents = 0;

    printf("Listening on %s\n", SGW_DEVICE);
    printf("Waiting for GPIO interrupts...\n");

    while(1)
    {
        ret = poll(&pfd, 1, -1);

        if(ret < 0)
        {
            if(errno == EINTR)
                continue;

            perror("poll");
            break;
        }

        if(pfd.revents & (POLLERR | POLLHUP | POLLNVAL))
        {
            fprintf(stderr,
                    "poll error: 0x%x\n",
                    pfd.revents);
            break;
        }

        if(!(pfd.revents & POLLIN))
            continue;

        n = read(fd, &event, sizeof(event));
        
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN)
                continue;

            perror("read");
            break;
        }

        if(n != sizeof(event))
        {
            fprintf(stderr,
                "Unecpected read size: %zd\n", n);
            break;
        }

        printf(
            "seq=%" PRIu32
            " level=%" PRIu32
            " state=%s"
            " time_ms=%" PRIu64 "\n",
            event.sequence,
            event.level,
            event.level == 0 ? "PRESSED" : "RELEASED",
            event.timestamp_ns / 1000000ULL
        );
    }

        close(fd);
        return EXIT_FAILURE;
}



