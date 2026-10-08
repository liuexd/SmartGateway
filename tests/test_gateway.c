#include "frame_parser.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

/*
 * 每解析出一个CRC正确的完整帧，
 * frame_parser_feed()会调用该函数。
 */
static void on_frame(
    const parsed_frame_t *frame,
    void *user_data
)
{
    frame_data_t data;

    (void)user_data;

    printf("\nReceived valid frame:\n");
    printf("%s", frame->raw);

    if (frame_decode_data(frame, &data) != 0)
    {
        printf("DATA frame decode failed\n");
        return;
    }

    printf("Node        : %s\n", data.node_id);
    printf(
        "Sequence    : %06u\n",
        (unsigned int)data.sequence
    );

    for (size_t i = 0; i < data.field_count; i++)
    {
        printf(
            "Field       : %s=%s\n",
            data.fields[i].key,
            data.fields[i].value
        );
    }

    /*
     * 温湿度可读输出（可选字段，存在才打印）。
     */
    {
        const char *t = frame_data_find_field(&data, "T");
        const char *h = frame_data_find_field(&data, "H");

        if (t != NULL && h != NULL)
        {
            printf(
                "Temperature : %.1f C\n",
                atoi(t) / 10.0
            );

            printf(
                "Humidity    : %.1f %%\n",
                atoi(h) / 10.0
            );
        }
    }
}

int main(int argc, char *argv[])
{
    const char *device = "/tmp/ttyGW";
    int fd;
    frame_parser_t parser;
    uint8_t read_buffer[128];

    if (argc >= 2)
    {
        device = argv[1];
    }

    fd = open(device, O_RDWR | O_NOCTTY);

    if (fd < 0)
    {
        perror("open gateway device");
        return 1;
    }

    printf("Gateway opened: %s\n", device);
    printf("Waiting for frames...\n");

    frame_parser_init(&parser);

    while (1)
    {
        ssize_t read_length;

        read_length = read(
            fd,
            read_buffer,
            sizeof(read_buffer)
        );

        if (read_length > 0)
        {
            /*
             * 不管read()返回半帧、一帧还是多帧，
             * 都直接交给流式解析器处理。
             */
            frame_parser_feed(
                &parser,
                read_buffer,
                (size_t)read_length,
                on_frame,
                NULL
            );

            continue;
        }

        if (read_length < 0 && errno == EINTR)
        {
            continue;
        }

        if (read_length == 0)
        {
            printf("Serial device closed\n");
        }
        else
        {
            perror("read");
        }

        break;
    }

    printf("\nParser statistics:\n");
    printf(
        "Valid frames   : %lu\n",
        parser.stats.valid_frames
    );

    printf(
        "CRC errors     : %lu\n",
        parser.stats.crc_errors
    );

    printf(
        "Format errors  : %lu\n",
        parser.stats.format_errors
    );

    printf(
        "Overflow errors: %lu\n",
        parser.stats.overflow_errors
    );

    close(fd);
    return 0;
}