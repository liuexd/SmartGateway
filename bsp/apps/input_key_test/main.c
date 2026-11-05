#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <linux/input.h>
#include <poll.h>

#define INPUT_DEVICE "/dev/input/event2"

int main(void)
{
    int fd;
    ssize_t n;
    struct input_event event;
    struct pollfd pfd;
    int ret;

    fd = open(INPUT_DEVICE,O_RDONLY);

    if(fd < 0){
        perror("open");
        return EXIT_FAILURE;
    }

    printf("Listening on %s\n", INPUT_DEVICE);

    pfd.fd = fd;
    pfd.events = POLLIN;
    pfd.revents = 0;

    while(1)
    {
        ret = poll (&pfd, 1, -1);

        if(ret < 0)
        {
            if(errno == EINTR)
                continue;

            perror("poll");
            break;
        }

        if(pfd.revents & (POLLERR|POLLHUP|POLLNVAL))
        {
            fprintf(stderr, "Input device error:0x%x\n",
                pfd.revents);
            break;
        }

        if(!(pfd.revents & POLLIN))
            continue;

        
        n = read(fd,&event,sizeof(event));

        if(n < 0)
        {
            if (errno == EINTR)
                continue;

            perror("read");
            break;
        }
            if (n != sizeof(event)) {
            fprintf(stderr, "Unexpected event size: %zd\n", n);
            break;
        }

        if (event.type != EV_KEY)
            continue;

        if (event.code == KEY_1) {
            printf("KEY1 %s\n",
                   event.value == 1 ? "PRESSED" :
                   event.value == 0 ? "RELEASED" : "REPEAT");
        }
        else if (event.code == KEY_2) {
            printf("KEY2 %s\n",
                   event.value == 1 ? "PRESSED" :
                   event.value == 0 ? "RELEASED" : "REPEAT");
        }
    }

    close(fd);
    return EXIT_FAILURE;
}