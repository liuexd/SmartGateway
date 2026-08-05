#define _POSIX_C_SOURCE 200809L

#include "gateway_app.h"
#include "message_json.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/*
 * 检查当前serial_port.c支持的波特率。
 */
static int baud_is_supported(int baud_rate)
{
    switch (baud_rate)
    {
        case 1200:
        case 2400:
        case 4800:
        case 9600:
        case 19200:
        case 38400:
        case 57600:
        case 115200:
            return 1;

        default:
            return 0;
    }
}

/*
 * 将端口号文本转换成整数。
 */
static int parse_port(
    const char *text,
    unsigned short *port
)
{
    char *end = NULL;
    long value;

    if (text == NULL ||
        port == NULL ||
        text[0] == '\0')
    {
        return -1;
    }

    errno = 0;

    value = strtol(text, &end, 10);

    if (errno != 0 ||
        end == text ||
        *end != '\0' ||
        value < 1 ||
        value > 65535)
    {
        return -1;
    }

    *port = (unsigned short)value;

    return 0;
}

/*
 * 将命令行中的波特率文本转换成整数。
 */
static int parse_baud_rate(
    const char *text,
    int *baud_rate
)
{
    char *end = NULL;
    long value;

    if (text == NULL ||
        baud_rate == NULL ||
        text[0] == '\0')
    {
        return -1;
    }

    errno = 0;

    value = strtol(text, &end, 10);

    if (errno != 0 ||
        end == text ||
        *end != '\0' ||
        value <= 0 ||
        value > 1000000L ||
        !baud_is_supported((int)value))
    {
        return -1;
    }

    *baud_rate = (int)value;

    return 0;
}

void gateway_app_init(
    gateway_context_t *context,
    const char *server_ip,
    unsigned short server_port
)
{
    if (context == NULL)
    {
        return;
    }

    memset(context, 0, sizeof(*context));

    context->tcp_fd = -1;
    context->serial_fd = -1;
    context->server_ip = server_ip;
    context->server_port = server_port;
    context->next_tcp_retry = 0;
}

int gateway_app_parse_args(
    int argc,
    char *argv[],
    const char **serial_device,
    int *baud_rate,
    gateway_context_t *context
)
{
    int i;

    if (serial_device == NULL ||
        baud_rate == NULL ||
        context == NULL)
    {
        return 1;
    }

    *serial_device = NULL;
    *baud_rate = 9600;

    for (i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "--serial") == 0)
        {
            if (i + 1 >= argc)
            {
                fprintf(stderr, "Missing value after --serial\n");
                return 1;
            }

            *serial_device = argv[++i];
        }
        else if (strcmp(argv[i], "--baud") == 0)
        {
            if (i + 1 >= argc ||
                parse_baud_rate(argv[i + 1], baud_rate) != 0)
            {
                fprintf(stderr, "Invalid or unsupported baud rate\n");
                return 1;
            }

            i++;
        }
        else if (strcmp(argv[i], "--server") == 0)
        {
            if (i + 1 >= argc)
            {
                fprintf(stderr, "Missing value after --server\n");
                return 1;
            }

            context->server_ip = argv[++i];
        }
        else if (strcmp(argv[i], "--port") == 0)
        {
            if (i + 1 >= argc ||
                parse_port(argv[i + 1], &context->server_port) != 0)
            {
                fprintf(stderr, "Invalid server port\n");
                return 1;
            }

            i++;
        }
        else if (strcmp(argv[i], "--help") == 0 ||
                 strcmp(argv[i], "-h") == 0)
        {
            return 1;
        }
        else
        {
            fprintf(stderr, "Unknown argument: %s\n", argv[i]);
            return 1;
        }
    }

    return 0;
}

void gateway_app_try_tcp_connect(gateway_context_t *context)
{
    time_t now;

    if (context == NULL)
    {
        return;
    }

    if (context->tcp_fd >= 0)
    {
        return;
    }

    now = time(NULL);

    if (now == (time_t)-1)
    {
        return;
    }

    /*
     * 距离上次尝试不足1秒，跳过本次重试。
     */
    if (now < context->next_tcp_retry)
    {
        return;
    }

    printf(
        "[TCP] connecting to %s:%u...\n",
        context->server_ip,
        (unsigned int)context->server_port
    );

    context->tcp_fd = tcp_client_connect(
        context->server_ip,
        context->server_port
    );

    if (context->tcp_fd < 0)
    {
        context->tcp_connect_error++;

        fprintf(
            stderr,
            "[TCP] tcp_connect failed: %s:%u\n",
            context->server_ip,
            (unsigned int)context->server_port
        );

        context->next_tcp_retry = now + 1;
        return;
    }

    context->tcp_connections++;

    printf(
        "[TCP] connected to %s:%u, connection = %lu\n",
        context->server_ip,
        (unsigned int)context->server_port,
        context->tcp_connections
    );
}

/*
 * 通过TCP将JSON发送到服务器。
 *
 * 未连接时丢弃并计数（后续可考虑缓存重发）。
 */
static void gateway_app_send_json(
    gateway_context_t *context,
    const char *json,
    int json_length
)
{
    if (context == NULL || json == NULL || json_length < 0)
    {
        return;
    }

    if (context->tcp_fd < 0)
    {
        context->tcp_dropped++;
        fprintf(stderr, "[TCP] not connected, message dropped\n");
        return;
    }

    if (tcp_client_send_all(
            context->tcp_fd,
            json,
            (size_t)json_length
        ) != 0)
    {
        context->tcp_send_error++;
        fprintf(stderr, "[TCP] send failed: %s\n", strerror(errno));

        tcp_client_close(context->tcp_fd);
        context->tcp_fd = -1;
        context->next_tcp_retry = time(NULL) + 1;
        return;
    }

    context->tcp_send++;
    printf("[TCP] JSON send, bytes = %u\n", (unsigned int)json_length);
}

/*
 * 每成功解析出一帧后，frame_parser_feed()
 * 会调用这个回调函数。
 *
 * 处理流程：
 * 1、根据帧类型分派（DATA/ACK/NACK）
 * 2、将帧解码成结构体
 * 3、输出打印
 * 4、通过snprintf生成JSON
 * 5、通过tcp连接发送数据到server
 */
void gateway_app_on_frame(
    const parsed_frame_t *frame,
    void *user_data
)
{
    gateway_context_t *context;
    frame_message_type_t type;
    char json[256];
    int json_length;

    if (frame == NULL || user_data == NULL)
    {
        return;
    }

    context = (gateway_context_t *)user_data;

    type = frame_get_message_type(frame);

    switch (type)
    {
        case FRAME_MESSAGE_DATA:
        {
            frame_data_t data;

            if (frame_decode_data(frame, &data) != 0)
            {
                context->decode_errors++;

                fprintf(
                    stderr,
                    "[PROTOCOL] DATA field decode failed: %s",
                    frame->raw
                );

                return;
            }

            printf(
                "[DATA] node=%s seq=%06u "
                "temperature=%.1f C humidity=%.1f %%\n",
                data.node_id,
                (unsigned int)data.sequence,
                data.temperature_x10 / 10.0,
                data.humidity_x10 / 10.0
            );

            json_length = message_json_build_data(
                json,
                sizeof(json),
                &data
            );

            if (json_length < 0)
            {
                fprintf(stderr, "[JSON] build failed\n");
                return;
            }

            gateway_app_send_json(context, json, json_length);
            break;
        }

        case FRAME_MESSAGE_ACK:
        {
            frame_ack_t ack;

            if (frame_decode_ack(frame, &ack) != 0)
            {
                context->decode_errors++;

                fprintf(
                    stderr,
                    "[PROTOCOL] ACK field decode failed: %s",
                    frame->raw
                );

                return;
            }

            printf(
                "[ACK] node=%s seq=%06u led=%d\n",
                ack.node_id,
                (unsigned int)ack.sequence,
                ack.led_value
            );

            json_length = message_json_build_ack(
                json,
                sizeof(json),
                &ack
            );

            if (json_length < 0)
            {
                fprintf(stderr, "[JSON] build failed\n");
                return;
            }

            gateway_app_send_json(context, json, json_length);
            break;
        }

        case FRAME_MESSAGE_NACK:
        {
            frame_nack_t nack;

            if (frame_decode_nack(frame, &nack) != 0)
            {
                context->decode_errors++;

                fprintf(
                    stderr,
                    "[PROTOCOL] NACK field decode failed: %s",
                    frame->raw
                );

                return;
            }

            printf(
                "[NACK] node=%s seq=%06u error=%s\n",
                nack.node_id,
                (unsigned int)nack.sequence,
                nack.error
            );

            json_length = message_json_build_nack(
                json,
                sizeof(json),
                &nack
            );

            if (json_length < 0)
            {
                fprintf(stderr, "[JSON] build failed\n");
                return;
            }

            gateway_app_send_json(context, json, json_length);
            break;
        }

        case FRAME_MESSAGE_CMD:
        case FRAME_MESSAGE_UNKNOWN:
        default:
            /*
             * 串口方向节点不应主动发CMD，
             * 未知帧只计数并丢弃。
             */
            context->decode_errors++;
            fprintf(
                stderr,
                "[PROTOCOL] unexpected frame type, ignored: %s",
                frame->raw
            );
            break;
    }
}

/*
 * 处理从TCP服务器收到的一行JSON（控制命令）。
 *
 * 流程：
 * 1、message_json_decode_command() 解码JSON -> frame_command_t
 * 2、frame_build_command() 组帧
 * 3、serial_port_write_all() 通过串口下发给节点
 */
void gateway_app_on_tcp_line(
    const char *line,
    size_t length,
    void *user_data
)
{
    gateway_context_t *context;
    frame_command_t command;
    char frame[FRAME_MAX_LEN];
    int frame_length;

    if (line == NULL || user_data == NULL)
    {
        return;
    }

    context = (gateway_context_t *)user_data;

    if (message_json_decode_command(
            line,
            length,
            &command
        ) < 0)
    {
        context->decode_errors++;

        fprintf(
            stderr,
            "[JSON] command decode failed: %.*s\n",
            (int)length,
            line
        );

        return;
    }

    printf(
        "[CMD] from server sender=%s seq=%06u led=%d\n",
        command.sender,
        (unsigned int)command.sequence,
        command.led_value
    );

    frame_length = frame_build_command(
        frame,
        sizeof(frame),
        command.sender,
        command.sequence,
        command.led_value
    );

    if (frame_length < 0)
    {
        fprintf(stderr, "[FRAME] build command failed\n");
        return;
    }

    if (context->serial_fd < 0)
    {
        fprintf(
            stderr,
            "[SERIAL] not connected, command dropped\n"
        );

        return;
    }

    if (serial_port_write_all(
            context->serial_fd,
            frame,
            (size_t)frame_length,
            1000
        ) != 0)
    {
        fprintf(stderr, "[SERIAL] write failed: %s\n", strerror(errno));
        return;
    }

    printf(
        "[SERIAL] command sent, bytes = %d\n",
        frame_length
    );
}

void gateway_app_print_stats(
    const gateway_context_t *context,
    const frame_parser_t *parser
)
{
    if (context == NULL || parser == NULL)
    {
        return;
    }

    printf("\nGateway stopped\n");

    printf("Valid frames    : %lu\n", parser->stats.valid_frames);
    printf("CRC errors      : %lu\n", parser->stats.crc_errors);
    printf("Format errors   : %lu\n", parser->stats.format_errors);
    printf("Overflow errors : %lu\n", parser->stats.overflow_errors);
    printf("Discarded bytes : %lu\n", parser->stats.discarded_bytes);
    printf("Decode errors   : %lu\n", context->decode_errors);
    printf("Serial connections : %lu\n", context->serial_connections);
    printf("TCP sent        : %lu\n", context->tcp_send);
    printf("TCP send errors : %lu\n", context->tcp_send_error);
    printf("TCP dropped     : %lu\n", context->tcp_dropped);
    printf("TCP connect errors : %lu\n", context->tcp_connect_error);
}

void gateway_app_close(gateway_context_t *context)
{
    if (context == NULL)
    {
        return;
    }

    if (context->tcp_fd >= 0)
    {
        tcp_client_close(context->tcp_fd);
        context->tcp_fd = -1;
    }
}
