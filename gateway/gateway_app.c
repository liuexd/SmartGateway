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
 * 通过TCP将JSON加入队列
 *
 * 未连接时丢弃并计数（后续可考虑缓存重发）。
 */
static void gateway_app_enqueue_json(
    message_queue_t *queue,
    const char *json,
    int json_length
)
{
    gateway_message_t message;
    int result;

    if (queue == NULL || json == NULL || json_length < 0)
    {
        return;
    }

    if((size_t)json_length > sizeof(message.data))
    {
        fprintf(stderr,
        "[QUEUE] json too large, byte = %d",json_length);
        return;
    }

    memset(&message,0,sizeof(message));

    memcpy(message.data,json,(size_t)json_length);

    message.length = (size_t)json_length;

    result = message_queue_push(queue,&message);

    if(result == MESSAGE_QUEUE_SHUTDOWN)
    {
        return;
    }

    if(result != MESSAGE_QUEUE_OK)
    {
        fprintf(stderr,
        "[QUEUE] push failed\n");
        return ;
    }

    printf("[QUEUE] upstream enqueued, byte=%d\n",json_length);
}

void gateway_app_enqueue_nack(
    message_queue_t *upstream_queue,
    const char *node_id,
    uint32_t sequence,
    const char *error
)
{
    frame_nack_t nack;

    char json[256];

    int json_length;

    if (upstream_queue == NULL ||
        node_id == NULL ||
        node_id[0] == '\0' ||
        error == NULL ||
        error[0] == '\0')
    {
        return;
    }


    if (strlen(node_id) >=
        sizeof(nack.node_id))
    {
        return;
    }


    if (strlen(error) >=
        sizeof(nack.error))
    {
        return;
    }


    memset(
        &nack,
        0,
        sizeof(nack)
    );


    strcpy(
        nack.node_id,
        node_id
    );


    nack.sequence =
        sequence;


    strcpy(
        nack.error,
        error
    );


    json_length =
        message_json_build_nack(
            json,
            sizeof(json),
            &nack
        );


    if (json_length < 0)
    {
        fprintf(
            stderr,
            "[NACK] JSON build failed, "
            "node=%s seq=%06u\n",
            node_id,
            (unsigned int)sequence
        );

        return;
    }

    gateway_app_enqueue_json(
        upstream_queue,
        json,
        json_length
    );

    printf(
        "[NACK] queued node=%s seq=%06u error=%s\n",
        node_id,
        (unsigned int)sequence,
        error
    );
}

static int gateway_app_touch_device(
    gateway_app_upstream_context_t *upstream_context,
    const char *node_id
)
{
    int result;

    if(upstream_context == NULL ||
        upstream_context->device_manager ==NULL ||
        node_id == NULL ||
        node_id[0] == '\0')
    {
        return -1;
    }

    /*
     *第一条合法消息
     *还没有绑定node_id
     */
    if(upstream_context->bound_node_id[0] == '\0')
    {
        size_t node_id_length;

        node_id_length = strlen(node_id);
        frame_command_t replaced_commands[DEVICE_COMMAND_QUEUE_CAPACITY];

        size_t replaced_count;
        size_t i;

        //比较的两段都可以换成字节数可以进行比较
        if(node_id_length >= sizeof(upstream_context->bound_node_id))
        {
            fprintf(stderr,
            "[DEVICE] node id too long\n");

            return -1;
        }

        replaced_count = 0U;


        result =
            device_manager_bind_connection_collect_commands(
                upstream_context->device_manager,
                node_id,
                upstream_context->transport,
                upstream_context->device_fd,
                pthread_self(),
                replaced_commands,
                DEVICE_COMMAND_QUEUE_CAPACITY,
                &replaced_count
        );

        if(result != DEVICE_MANAGER_OK)
        {
            fprintf(
                stderr,
                "[DEVICE] register failed, node=%s, result=%d\n",
                node_id,
                result);

            return -1;
        }

        for (i = 0U;
            i < replaced_count;
            i++)
        {
            gateway_app_enqueue_nack(
                upstream_context->upstream_queue,
                replaced_commands[i].target_node,
                replaced_commands[i].sequence,
                "transport_replaced"
            );
        }
        if (replaced_count > 0U)
        {
            printf(
                "[DEVICE] connection takeover node=%s, "
                "failed %zu pending command(s)\n",
                node_id,
                replaced_count
            );
        }

        /*
         * 记录本连接的身份，后续帧必须与之一致。
         *
         * memcpy 已带上结尾的 '\0'（node_id_length + 1U），
         * 因此这里不需要额外的 strcpy。
         */
        memcpy(upstream_context->bound_node_id,node_id,node_id_length+1U);

        printf("[DEVICE] register node=%s, transport=%s, fd=%d\n",node_id,
            (upstream_context->transport == DEVICE_TRANSPORT_BLUETOOTH)
                ? "BLUETOOTH" : "WIFI",
            upstream_context->device_fd);

        return 0;
    }
    //一个物理连接不能运行中突然冒充另一个node
    if(strcmp(upstream_context->bound_node_id,node_id) != 0)
    {
        fprintf(
            stderr,
            "[DEVICE] identity mismatch: bound=%s, received=%s\n",
            upstream_context->bound_node_id,
            node_id
        );

        return -1;
    }

    result = device_manager_update_last_seen(
        upstream_context->device_manager,
        node_id
    );

    if(result != DEVICE_MANAGER_OK)
    {
        fprintf(
            stderr,
            "[DEVICE] update last_seen failed, node=%s",
            node_id
        );
        return -1;
    }
    return 0;
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
    gateway_app_upstream_context_t *upstream_context;
    gateway_context_t *context;
    message_queue_t *upstream_queue;

    frame_message_type_t type;
    char json[256];
    int json_length;

    if (frame == NULL || user_data == NULL)
    {
        return;
    }

    upstream_context = (gateway_app_upstream_context_t *)user_data;
    context = upstream_context->gateway_context;
    upstream_queue = upstream_context->upstream_queue;

    if(context == NULL ||upstream_queue == NULL)
    {
        return ;
    }

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

            if(gateway_app_touch_device(upstream_context,data.node_id)!=0)
            {
                return;
            }

            printf(
                "[DATA] node=%s seq=%06u fields=",
                data.node_id,
                (unsigned int)data.sequence
            );

            for (size_t i = 0; i < data.field_count; i++)
            {
                printf(
                    "%s%s=%s",
                    (i > 0U) ? "," : "",
                    data.fields[i].key,
                    data.fields[i].value
                );
            }

            printf("\n");

            /*
             * 温湿度可读输出（可选字段，存在才打印）。
             */
            {
                const char *t = frame_data_find_field(&data, "T");
                const char *h = frame_data_find_field(&data, "H");

                if (t != NULL && h != NULL)
                {
                    printf(
                        "        temperature=%.1f C humidity=%.1f %%\n",
                        atoi(t) / 10.0,
                        atoi(h) / 10.0
                    );
                }
            }

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

            gateway_app_enqueue_json(upstream_queue, json, json_length);
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

            if(gateway_app_touch_device(upstream_context,ack.node_id)!=0)
            {
                return;
            }

            printf(
                "[ACK] node=%s seq=%06u fields=",
                ack.node_id,
                (unsigned int)ack.sequence
            );

            for (size_t i = 0; i < ack.field_count; i++)
            {
                printf(
                    "%s%s=%s",
                    (i > 0U) ? "," : "",
                    ack.fields[i].key,
                    ack.fields[i].value
                );
            }

            printf("\n");

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

            gateway_app_enqueue_json(upstream_queue, json, json_length);
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

            if(gateway_app_touch_device(upstream_context,nack.node_id)!=0)
            {
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

            gateway_app_enqueue_json(upstream_queue, json, json_length);
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
 * 路由失败时，由网关代理目标节点回一条 NACK。
 *
 * 使用场景：
 *   命令已经无法送达节点（目标不存在 / 已离线 / 该节点命令队列满），
 *   如果什么都不做，Server 只能干等到 CMD_TIMEOUT_SEC 才判定超时，
 *   期间还会盲目重发若干次。
 *
 *   网关比 Server 更清楚南向链路的真实状态，
 *   所以这里主动回一条 NACK，让 Server 的 command_manager
 *   立刻把该命令标记为 NACKED，而不是白等一轮超时。
 *
 * 关键点：
 *   nack.node_id 必须填**原命令的目标节点**，不能填发送方 GATEWAY。
 *   Server 侧 command_manager_on_nack() 会校验 seq + node_id
 *   是否与在途命令匹配，填错会被当成伪造应答而拒绝。
 *
 * 线程说明：
 *   本函数只会在 Server Link Thread 中执行（gateway_app_on_tcp_line 的
 *   调用者），而 tcp_fd 的拥有者正是该线程，
 *   因此这里直接同步写 tcp_fd 是安全的，不构成跨线程写冲突。
 *
 * @param context 网关上下文（提供 tcp_fd）
 * @param command 原命令（提供 target_node 与 sequence）
 * @param error   失败原因，如 "route_offline"
 *
 * @return 0已回复；-1参数非法或发送失败
 */
static int gateway_app_send_route_nack(
    gateway_context_t *context,
    const frame_command_t *command,
    const char *error
)
{
    frame_nack_t nack;

    char json[256];

    int json_length;

    if (context == NULL ||
        command == NULL ||
        error == NULL ||
        error[0] == '\0')
    {
        return -1;
    }


    if (context->tcp_fd < 0)
    {
        return -1;
    }


    memset(
        &nack,
        0,
        sizeof(nack)
    );

    /*
     * 注意：
     * 这里的node_id必须填写原命令的目标节点。
     *
     * Server的command_manager才能通过：
     *
     * sequence + node_id
     *
     * 找到正确命令。
     */
    strcpy(nack.node_id,command->target_node);

    nack.sequence = command->sequence;

    if (strlen(error) >=
        sizeof(nack.error))
    {
        return -1;
    }

    strcpy(
        nack.error,
        error
    );

    json_length = message_json_build_nack(
            json,
            sizeof(json),
            &nack
    );

    if (json_length < 0)
    {
        fprintf(
            stderr,
            "[ROUTE NACK] JSON build failed\n"
        );

        return -1;
    }

    /*
     * 当前函数由Server Link Thread执行，
     * 因此可以同步回复当前Server连接。
     */
    if (tcp_client_send_all(
            context->tcp_fd,
            json,
            (size_t)json_length
        ) != 0)
    {
        context->tcp_send_error++;

        fprintf(
            stderr,
            "[ROUTE NACK] send failed, "
            "target=%s seq=%06u error=%s\n",
            command->target_node,
            (unsigned int)command->sequence,
            error
        );

        return -1;
    }

    context->tcp_send++;

    printf(
        "[ROUTE NACK] target=%s seq=%06u error=%s\n",
        command->target_node,
        (unsigned int)command->sequence,
        error
    );


    return 0;

}
/*
 * 处理从TCP服务器收到的一行JSON（控制命令）。
 *
 * 流程：
 * 1、message_json_decode_command() 解码JSON -> frame_command_t
 * 2、device_manager_enqueue_command() 按目标节点投递到该设备的命令队列
 *
 * 本函数运行在 Server Link Thread，不直接操作串口：
 * 串口 fd 唯一属于 bluetooth_worker，
 * 由它从队列取出命令后再写入。
 */
void gateway_app_on_tcp_line(
    const char *line,
    size_t length,
    void *user_data
)
{
    gateway_app_downlink_context_t *downlink_context;

    gateway_context_t *context;
    device_manager_t *device_manager;

    frame_command_t command;

    int result;

    if (line == NULL || user_data == NULL)
    {
        return;
    }

    downlink_context = (gateway_app_downlink_context_t *)user_data;

    context = downlink_context->gateway_context;

    device_manager = downlink_context->device_manager;

    if(context == NULL ||
        device_manager == NULL)
    {
        return;
    }

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
        "[CMD] target=%s sender=%s seq=%06u fields=",
        command.target_node,
        command.sender,
        (unsigned int)command.sequence
    );

    for (size_t i = 0; i < command.field_count; i++)
    {
        printf(
            "%s%s=%s",
            (i > 0U) ? "," : "",
            command.fields[i].key,
            command.fields[i].value
        );
    }

    printf("\n");

    result = device_manager_enqueue_command(
        device_manager,
        command.target_node,
        &command
    );

    switch (result)
    {
        case DEVICE_MANAGER_OK:

            printf(
                "[ROUTE] target=%s seq=%06u queued\n",
                command.target_node,
                (unsigned int)command.sequence
            );

            break;


        case DEVICE_MANAGER_NOT_FOUND:

            fprintf(
                stderr,
                "[ROUTE] target=%s not found\n",
                command.target_node
            );

            gateway_app_send_route_nack(
                context,
                &command,
                "route_not_found"
            );

            break;


        case DEVICE_MANAGER_OFFLINE:

            fprintf(
                stderr,
                "[ROUTE] target=%s offline\n",
                command.target_node
            );

            gateway_app_send_route_nack(
                context,
                &command,
                "route_offline"
            );

            break;


        case DEVICE_MANAGER_QUEUE_FULL:

            fprintf(
                stderr,
                "[ROUTE] target=%s command queue full\n",
                command.target_node
            );

            gateway_app_send_route_nack(
                context,
                &command,
                "route_queue_full"
            );

            break;


        default:

            fprintf(
                stderr,
                "[ROUTE] enqueue failed, "
                "target=%s result=%d\n",
                command.target_node,
                result
            );

            gateway_app_send_route_nack(
                context,
                &command,
                "route_internal"
            );

            break;
    }

}

void gateway_app_on_wifi_line(
    const char *line,
    size_t length,
    void *user_data
)
{
    gateway_app_upstream_context_t *upstream_context;
    gateway_context_t *context;
    message_queue_t *upstream_queue;

    char type_name[8];

    if(line == NULL || user_data == NULL)
    {
        return;
    }

    upstream_context = (gateway_app_upstream_context_t *)user_data;
    context = upstream_context->gateway_context;
    upstream_queue = upstream_context->upstream_queue;

    if(context == NULL || upstream_context == NULL)
    {
        return ;
    }

    /*
     * WiFi 节点和蓝牙节点一样会回 DATA / ACK / NACK 三类，
     * 必须按 type 分派。
     *
     * 之前这里直接调 message_json_decode_data()，
     * 而它硬校验 type=="DATA"，
     * 导致 WiFi 的 ACK/NACK 全部解析失败被丢弃，
     * Server 侧命令永远等不到应答、只能反复超时重发。
     */
    if (message_json_peek_type(
            line,
            length,
            type_name,
            sizeof(type_name)) != 0)
    {
        context->decode_errors++;

        fprintf(stderr,
            "[WIFI] cannot read message type: %.*s\n",
            (int)length,
            line
        );

        return;
    }

    if (strcmp(type_name, "ACK") == 0)
    {
        frame_ack_t ack;
        char ack_json[256];
        int ack_length;

        if (message_json_decode_ack(line, length, &ack) < 0)
        {
            context->decode_errors++;

            fprintf(stderr,
                "[WIFI] ACK decode failed: %.*s\n",
                (int)length,
                line
            );

            return;
        }

        if (gateway_app_touch_device(
                upstream_context,
                ack.node_id) != 0)
        {
            return;
        }

        printf(
            "[WIFI ACK] node=%s seq=%06u fields=",
            ack.node_id,
            (unsigned int)ack.sequence
        );

        for (size_t i = 0; i < ack.field_count; i++)
        {
            printf(
                "%s%s=%s",
                (i > 0U) ? "," : "",
                ack.fields[i].key,
                ack.fields[i].value
            );
        }

        printf("\n");

        ack_length = message_json_build_ack(
            ack_json,
            sizeof(ack_json),
            &ack
        );

        if (ack_length < 0)
        {
            fprintf(stderr, "[WIFI] ACK JSON build failed\n");
            return;
        }

        gateway_app_enqueue_json(
            upstream_queue,
            ack_json,
            ack_length
        );

        return;
    }

    if (strcmp(type_name, "NACK") == 0)
    {
        frame_nack_t nack;
        char nack_json[256];
        int nack_length;

        if (message_json_decode_nack(line, length, &nack) < 0)
        {
            context->decode_errors++;

            fprintf(stderr,
                "[WIFI] NACK decode failed: %.*s\n",
                (int)length,
                line
            );

            return;
        }

        if (gateway_app_touch_device(
                upstream_context,
                nack.node_id) != 0)
        {
            return;
        }

        printf(
            "[WIFI NACK] node=%s seq=%06u error=%s\n",
            nack.node_id,
            (unsigned int)nack.sequence,
            nack.error
        );

        nack_length = message_json_build_nack(
            nack_json,
            sizeof(nack_json),
            &nack
        );

        if (nack_length < 0)
        {
            fprintf(stderr, "[WIFI] NACK JSON build failed\n");
            return;
        }

        gateway_app_enqueue_json(
            upstream_queue,
            nack_json,
            nack_length
        );

        return;
    }

    /*
     * 其余按 DATA 处理（含 type 不是 DATA 的未知类型，
     * 由 decode_data 自己校验并报错）。
     */
    {
        frame_data_t data;
        const char *light_raw;
        char json[256];
        int json_length;

        if(message_json_decode_data(line,length,&data) < 0)
        {
            context ->decode_errors++;

            fprintf(stderr,
            "[WIFI] DATA decode failed: %.*s\n",
            (int)length,
            line
            );

            return;
        }

        if (gateway_app_touch_device(upstream_context,data.node_id
        ) != 0)
        {
            return;
        }

        printf(
            "[WIFI DATA] node=%s seq=%06u fields=",
            data.node_id,
            data.sequence
        );

        for(size_t i=0; i<data.field_count;i++)
        {
            printf("%s%s=%s",
            (i > 0U) ? "," :"",//添加中间的逗号
            data.fields[i].key,
            data.fields[i].value
            );
        }

        printf("\n");

        light_raw = frame_data_find_field(
            &data,"LIGHT_RAW"
            );

        if(light_raw != NULL)
        {
            printf(
                "            LIGHT_RAW=%s\n",
                light_raw
            );
        }

        /*
         *将解码后的WIFIDATA重新序列化为标准JSON
         *再通过现有的TCP发送给服务器
         */

        json_length = message_json_build_data(
            json,
            sizeof(json),
            &data
        );

        if(json_length < 0)
        {
            fprintf(stderr,
                "[WIFI] JSON build failed\n"
            );
            return;
        }

        gateway_app_enqueue_json(
            upstream_queue,
            json,
            json_length
        );
    }
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
