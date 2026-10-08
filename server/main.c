#define _POSIX_C_SOURCE 200809L

#include "command_manager.h"
#include "line_parser.h"
#include "message_json.h"
#include "node_store.h"
#include "tcp_server.h"

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_SERVER_PORT 9000
#define RECEIVE_BUFFER_SIZE 4096
#define TEMP_BUFFER_SIZE    256
#define NODE_STORE_MAX_NODES 64
#define CMD_BUFFER_SIZE     256
#define CMD_SENDER          "GATEWAY"
#define CMD_TIMEOUT_SEC     3     /* 命令应答超时阈值（秒） */
#define CMD_MAX_RETRIES     2     /* 超时最大重试次数 */
#define CMD_RETRY_TABLE     16    /* 单次超时检查的输出容量 */


/* 节点注册表打印周期（秒） */
#define REGISTRY_PRINT_INTERVAL 10

/* 节点在线判定阈值（秒）：超过该时长未上报视为离线 */
#define NODE_ONLINE_TIMEOUT_SEC 30

/*
 * on_line 回调上下文：
 * 打包节点存储与命令管理器，供回调使用。
 */
typedef struct
{
    node_store_t *store;
    command_manager_t *manager;
} server_context_t;

static volatile sig_atomic_t g_running = 1;

/*
信号处理函数
 */
static void handle_signal(int signal_num)
{
    (void)signal_num; // 未使用到避免警告，同时由于信号处理函数符合固定签名。所以必须传入int
    g_running = 0;
}

/*
安装SIGINT和SIGTERM信号处理函数
*/
static int install_signal_handlers(void)
{
    struct sigaction action;

    memset(&action, 0, sizeof(action));

    action.sa_handler = handle_signal;
    sigemptyset(&action.sa_mask);

    if (sigaction(SIGINT, &action, NULL) != 0)
    {
        return -1;
    }
    if (sigaction(SIGTERM, &action, NULL) != 0)
    {
        return -1;
    }
    return 0;
}
/*
将端口号转换成整数
*/
static int parse_port(const char *text, unsigned short *port)
{
    char *end = NULL;
    long int value;

    if (text == NULL || text[0] == '\0' || port == NULL)
    {
        return -1;
    }
    errno = 0;
    value = strtol(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value < 1 || value > 65535)
    {
        return -1;
    }
    *port = (uint16_t)value;
    return 0;
}

/*
 * 判断一行（长度限定，不保证'\0'结尾）中是否包含子串。
 *
 * line_parser 传给回调的 line 指向缓冲区中间，
 * 后面可能还有半条消息，不能用 strstr()。
 */
static int line_has(
    const char *line,
    size_t length,
    const char *pattern)
{
    size_t pattern_length = strlen(pattern);
    size_t i;

    if (pattern_length > length)
    {
        return 0;
    }

    for (i = 0; i + pattern_length <= length; i++)
    {
        if (memcmp(line + i, pattern, pattern_length) == 0)
        {
            return 1;
        }
    }

    return 0;
}

/*
 * 每收到一条完整消息（以'\n'结尾）时调用。
 * 由 line_parser_feed() 触发。
 *
 * 处理流程：
 * 1. 按类型区分消息：
 *    - ACK：解析后交给 command_manager_on_ack() 更新命令状态；
 *    - NACK：解析后交给 command_manager_on_nack() 更新命令状态；
 *    - DATA：解析为 frame_data_t，更新节点存储。
 */
static void on_line(
    const char *line,
    size_t length,
    void *user_data
)
{
    server_context_t *ctx;
    frame_data_t data;
    int consumed;

    if (user_data == NULL)
    {
        return;
    }

    ctx = (server_context_t *)user_data;

    printf(
        "[Message] Length=%zu %.*s\n",
        length,
        (int)length,
        line
    );

    /*
     * 网关回传的ACK：命令执行成功。
     */
    if (line_has(line, length, "\"type\":\"ACK\""))
    {
        frame_ack_t ack;

        if (message_json_decode_ack(line, length, &ack) < 0)
        {
            fprintf(
                stderr,
                "[Server] ACK JSON decode failed\n"
            );
            return;
        }

        if (command_manager_on_ack(ctx->manager, &ack) == 0)
        {
            printf(
                "[CMD-MGR] seq=%u -> ACKED\n",
                (unsigned int)ack.sequence
            );
        }
        else
        {
            printf(
                "[CMD-MGR] seq=%u ACK unmatched, ignored\n",
                (unsigned int)ack.sequence
            );
        }
        return;
    }

    /*
     * 网关回传的NACK：命令执行失败。
     */
    if (line_has(line, length, "\"type\":\"NACK\""))
    {
        frame_nack_t nack;

        if (message_json_decode_nack(line, length, &nack) < 0)
        {
            fprintf(
                stderr,
                "[Server] NACK JSON decode failed\n"
            );
            return;
        }

        if (command_manager_on_nack(ctx->manager, &nack) == 0)
        {
            printf(
                "[CMD-MGR] seq=%u -> NACKED (%s)\n",
                (unsigned int)nack.sequence,
                nack.error
            );
        }
        else
        {
            printf(
                "[CMD-MGR] seq=%u NACK unmatched, ignored\n",
                (unsigned int)nack.sequence
            );
        }
        return;
    }

    consumed = message_json_decode_data(line, length, &data);

    if (consumed < 0)
    {
        fprintf(
            stderr,
            "[NodeStore] DATA JSON decode failed: %.*s\n",
            (int)length,
            line
        );
        return;
    }

    if (node_store_update(ctx->store, &data) != 0)
    {
        fprintf(
            stderr,
            "[NodeStore] update failed: %s\n",
            data.node_id
        );
        return;
    }

    {
        frame_device_type_t dev_type;

        dev_type = frame_device_type_from_text(
            frame_data_find_field(&data, FRAME_KV_DEV));

        printf(
            "[NodeStore] node=%s seq=%u dev=%s fields=",
            data.node_id,
            (unsigned int)data.sequence,
            frame_device_type_name(dev_type)
        );
    }

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
                "           temperature=%.1f C humidity=%.1f %%\n",
                atoi(t) / 10.0,
                atoi(h) / 10.0
            );
        }
    }
}

/*
 * 保证把全部数据发送给客户端。
 *
 * 循环send直至全部发出；用MSG_NOSIGNAL避免对端断开时触发SIGPIPE。
 */
static int send_all(
    int fd,
    const void *data,
    size_t length)
{
    const uint8_t *bytes = (const uint8_t *)data;
    size_t total = 0;

    while (total < length)
    {
        ssize_t sent = send(
            fd,
            bytes + total,
            length - total,
            MSG_NOSIGNAL
        );

        if (sent > 0)
        {
            total += (size_t)sent;
            continue;
        }

        if (sent < 0 && errno == EINTR)
        {
            continue;
        }

        return -1;
    }

    return 0;
}

/*
 * 节点注册表遍历回调。
 *
 * 为每个已注册节点打印一行：node_id、设备类型、在线状态、最近上报时间。
 */
static int print_registry_entry(
    const frame_data_t *data,
    time_t last_update,
    void *user_data)
{
    frame_device_type_t dev_type;
    int online;
    const node_store_t *store;
    struct tm tm_buf;
    char time_text[32];

    (void)user_data;

    store = (const node_store_t *)user_data;

    dev_type = frame_device_type_from_text(
        frame_data_find_field(data, FRAME_KV_DEV));

    if (node_store_is_online(store, data->node_id,
                             NODE_ONLINE_TIMEOUT_SEC, &online) != 0)
    {
        online = 0;
    }

    if (localtime_r(&last_update, &tm_buf) != NULL)
    {
        strftime(time_text, sizeof(time_text), "%H:%M:%S", &tm_buf);
    }
    else
    {
        strcpy(time_text, "????");
    }

    printf(
        "  %-8s %-10s %s  last=%s\n",
        data->node_id,
        frame_device_type_name(dev_type),
        online ? "[ONLINE ]" : "[OFFLINE]",
        time_text
    );

    return 0;
}

/*
 * 打印当前节点注册表。
 *
 * 列出所有已注册节点及其设备类型/在线状态。
 */
static void print_node_registry(const node_store_t *store)
{
    printf("\n--- Node registry ---\n");
    node_store_foreach(store, print_registry_entry, (void *)store);
    printf("---------------------\n");
}

/*
 * 从stdin读取一行命令，下发到网关。
 *
 * 支持：
 *   led <node> <value>   -> 下发LED控制命令到指定节点（M7-7C4-2 起）
 *   nodes                -> 打印当前节点注册表
 *   help                 -> 打印帮助
 *
 * 目标节点必须由用户显式指定，Server 不再猜默认节点。
 * Server 只负责"按用户指定的 target 构造并发送 CMD"；
 * 节点是否存在、是否离线、队列是否满，
 * 由 Gateway 的 Device Manager 判断并回 NOT_FOUND / OFFLINE / QUEUE_FULL。
 * （Server 的 node_store 只根据 DATA 推测在线状态，与 Gateway 的真实连接
 *   状态语义不同，因此这里不做在线预判。）
 *
 * LED命令内部转换为通用键值对（LED=value），
 * 后续接入更多设备类型时，可扩展为任意 KEY=VALUE 参数。
 *
 * 命令序号由 command_manager 统一分配，并记录为等待应答状态。
 * 目标节点也会一并交给 command_manager 保存，
 * 这样超时重发时才能重建正确的路由目标。
 *
 * @param client_fd 网关连接socket（<0表示未连接）
 * @param manager   命令管理器
 * @param store     节点存储（用于 nodes 命令）
 */
static void handle_stdin_command(
    int client_fd,
    command_manager_t *manager,
    node_store_t *store)
{
    char command[CMD_BUFFER_SIZE];
    char target_node[COMMAND_TARGET_NODE_SIZE];
    int led_value = -1;
    frame_kv_t fields[1];
    uint32_t sequence;

    /*
     * 用 %31s 把目标节点与 LED 值都读成字符串再自行校验，
     * 而不是直接写 "led %31s %d"：
     *   - 目标节点长度受 31 限制，不会写爆 target_node[32]；
     *   - LED 值在这里用 strtol 解析，能拒绝 "1abc" 这种脏输入；
     *   - 末尾的 %c 用来探测多余 token。
     */
    char led_text[32];
    char tail;
    int matched;

    if (fgets(command, sizeof(command), stdin) == NULL)
    {
        return;
    }

    /* 去掉结尾的\r\n */
    command[strcspn(command, "\r\n")] = '\0';

    if (strcmp(command, "help") == 0)
    {
        printf(
            "Commands:\n"
            "  led <node> <value>\n"
            "  nodes\n"
            "  help\n"
        );
        return;
    }

    if (strcmp(command, "nodes") == 0)
    {
        print_node_registry(store);
        return;
    }

    memset(target_node, 0, sizeof(target_node));
    memset(led_text, 0, sizeof(led_text));
    tail = '\0';

    /*
     * 期望恰好 2 个转换：目标节点 + LED 值。
     *
     * 末尾用 %c（而不是 %1s）探测多余 token：
     *   %1s 会跳过空白再读，没有内容时同样失败，
     *   于是 "led N1 1" 和 "led N1 1 abc" 的返回值恰好相反。
     *   %c 不跳过空白并且需要一个字符：
     *     "led N1 1"      -> 无剩余字符 -> matched = 2  ✓
     *     "led N1 1 abc"  -> 读到 'a'   -> matched = 3  ✗拒绝
     */
    matched = sscanf(
        command,
        "led %31s %31s %c",
        target_node,
        led_text,
        &tail
    );

    if (matched != 2)
    {
        fprintf(
            stderr,
            "[Server] unknown command: %s "
            "(try: led NODE01 1)\n",
            command
        );
        return;
    }

    /*
     * LED 值必须是完整数字。
     */
    {
        char *end = NULL;
        long value;

        errno = 0;

        value = strtol(led_text, &end, 10);

        if (errno != 0 ||
            end == led_text ||
            *end != '\0' ||
            value < 0 ||
            value > 1)
        {
            fprintf(
                stderr,
                "[Server] invalid LED value: %s (expect 0 or 1)\n",
                led_text
            );
            return;
        }

        led_value = (int)value;
    }

    {
        frame_command_t cmd;
        char json[256];
        int json_length;

        if (client_fd < 0)
        {
            fprintf(
                stderr,
                "[Server] no gateway connected, command dropped\n"
            );
            return;
        }

        /*
         * LED参数转换为通用键值对。
         */
        snprintf(fields[0].key, sizeof(fields[0].key), "LED");
        snprintf(fields[0].value, sizeof(fields[0].value), "%d", led_value);

        /*
         * 分配序号并记录为等待应答。
         * 目标节点一并交给 command_manager 保存，
         * 超时重发时才能重建正确的路由目标。
         */
        if (command_manager_send(
                manager,
                target_node,
                CMD_SENDER,
                fields,
                1U,
                &sequence
            ) != 0)
        {
            fprintf(
                stderr,
                "[Server] command table full, command dropped\n"
            );
            return;
        }

        memset(&cmd, 0, sizeof(cmd));

        /*
         * target_node 是命令的目标节点：
         * 网关侧 device_manager 靠它路由到对应链路，
         * 缺了它就会被判为非法目标而丢弃。
         */
        strcpy(cmd.target_node, target_node);
        strcpy(cmd.sender, CMD_SENDER);
        cmd.sequence = sequence;
        memcpy(cmd.fields, fields, sizeof(fields));
        cmd.field_count = 1;

        json_length = message_json_build_command(
            json,
            sizeof(json),
            &cmd
        );

        if (json_length < 0)
        {
            fprintf(stderr, "[Server] build CMD JSON failed\n");
            return;
        }

        if (send_all(client_fd, json, (size_t)json_length) != 0)
        {
            fprintf(stderr, "[Server] send CMD failed\n");
            return;
        }

        printf(
            "[Server] CMD sent target=%s seq=%u: %s",
            target_node,
            (unsigned int)sequence,
            json
        );
        return;
    }
}

int main(int argc, char **argv)
{
    unsigned short port = DEFAULT_SERVER_PORT;
    int server_fd = -1;
    int client_fd = -1;

    node_store_t *store;
    command_manager_t *manager;
    server_context_t server_ctx;
    char receive_buffer[RECEIVE_BUFFER_SIZE];
    size_t receive_length = 0;
    time_t next_registry_print = 0;

    if(argc>=2)
    {
        //手动设置端口
        if(parse_port(argv[1],&port)!=0)
        {
            fprintf(stderr,"Invalid port: %s\n",argv[1]);
            return 1;
        }
    }

    store = node_store_create(NODE_STORE_MAX_NODES);
    if(store == NULL)
    {
        fprintf(stderr,"node_store_create failed\n");
        return 1;
    }

    manager = command_manager_create();
    if(manager == NULL)
    {
        fprintf(stderr,"command_manager_create failed\n");
        node_store_destroy(store);
        return 1;
    }

    server_ctx.store = store;
    server_ctx.manager = manager;

    if(install_signal_handlers()!=0)
    {
        perror("sigaction");
        command_manager_destroy(manager);
        node_store_destroy(store);
        return 1;
    }
    server_fd = tcp_server_create(port);
    if(server_fd <0)
    {
        perror("create socket error");
        command_manager_destroy(manager);
        node_store_destroy(store);
        return 1;
    }
    printf("TCP server listening on 127.0.0.1:%u\n",(unsigned int)port);
    printf("Press Ctrl+C to stop.\n");
    printf("Commands: led <node> <value> | nodes | help\n");
    while(g_running)
    {
        struct pollfd pollfd[3];
        nfds_t poll_count;
        int poll_result;
        time_t now;

        uint32_t retry_sequences[CMD_RETRY_TABLE];
        char retry_target_nodes[CMD_RETRY_TABLE][COMMAND_TARGET_NODE_SIZE];
        frame_kv_t retry_fields[CMD_RETRY_TABLE][FRAME_DATA_MAX_FIELDS];
        size_t retry_field_counts[CMD_RETRY_TABLE];
        uint32_t timeout_sequences[CMD_RETRY_TABLE];
        size_t retry_count = 0;
        size_t timeout_count = 0;
        size_t i;

        /*
         * 周期性打印节点注册表（每 REGISTRY_PRINT_INTERVAL 秒）。
         */
        now = time(NULL);

        if (now >= next_registry_print)
        {
            print_node_registry(store);
            next_registry_print = now + REGISTRY_PRINT_INTERVAL;
        }

        /*
         * 检查超时未应答的命令并重发。
         */
        command_manager_check_timeouts(
            manager,
            now,
            CMD_TIMEOUT_SEC,
            CMD_MAX_RETRIES,
            retry_sequences,
            retry_target_nodes,
            retry_fields,
            retry_field_counts,
            CMD_RETRY_TABLE,
            &retry_count,
            timeout_sequences,
            CMD_RETRY_TABLE,
            &timeout_count
        );

        for (i = 0; i < retry_count; i++)
        {
            frame_command_t cmd;
            char json[256];
            int json_length;

            if (client_fd < 0)
            {
                fprintf(
                    stderr,
                    "[Server] no gateway connected, "
                    "CMD seq=%u retry dropped\n",
                    (unsigned int)retry_sequences[i]
                );
                continue;
            }

            memset(&cmd, 0, sizeof(cmd));

            /*
             * 重发同样要带目标节点，
             * 否则网关会因目标为空而丢弃这条命令。
             */
            snprintf(
                cmd.target_node,
                sizeof(cmd.target_node),
                "%s",
                retry_target_nodes[i]
            );

            strcpy(cmd.sender, CMD_SENDER);
            cmd.sequence = retry_sequences[i];
            memcpy(cmd.fields, retry_fields[i], sizeof(retry_fields[i]));
            cmd.field_count = retry_field_counts[i];

            json_length = message_json_build_command(
                json,
                sizeof(json),
                &cmd
            );

            if (json_length < 0 ||
                send_all(client_fd, json, (size_t)json_length) != 0)
            {
                fprintf(
                    stderr,
                    "[Server] CMD seq=%u retry failed\n",
                    (unsigned int)retry_sequences[i]
                );
                continue;
            }

            printf(
                "[Server] CMD target=%s seq=%u retried: %s",
                cmd.target_node,
                (unsigned int)retry_sequences[i],
                json
            );
        }

        for (i = 0; i < timeout_count; i++)
        {
            printf(
                "[Server] CMD seq=%u TIMEOUT, retries exhausted\n",
                (unsigned int)timeout_sequences[i]
            );
        }

        pollfd[0].fd = server_fd;
        pollfd[0].events = POLLIN;
        pollfd[0].revents = 0;
        poll_count = 1;

        if(client_fd >=0)
        {
            pollfd[1].fd=client_fd;
            pollfd[1].events=POLLIN;
            pollfd[1].revents =0;

            poll_count=2;
        }

        /*
         * 监听stdin，支持手动下发命令。
         */
        pollfd[poll_count].fd = STDIN_FILENO;
        pollfd[poll_count].events = POLLIN;
        pollfd[poll_count].revents = 0;
        poll_count++;

        poll_result = poll(pollfd,poll_count,1000);

        if(poll_result<0)
        {
            if(errno == EINTR)
            {
                continue;
            }
            perror("poll");
            break;
        }
        if(poll_result == 0)
        {
            
            continue;
        }

        /*
         * 处理stdin输入的命令。
         */
        {
            int stdin_index = poll_count - 1;

            if ((pollfd[stdin_index].revents & POLLIN) != 0)
            {
                handle_stdin_command(client_fd, manager, store);
            }
        }
        //出现新的连接
        if((pollfd[0].revents&POLLIN)!=0)
        {
            char client_ip[64];
            unsigned short client_port = 0;
            int new_client_fd;

            new_client_fd = tcp_server_accept(
                server_fd,
                client_ip,
                sizeof(client_ip),
                &client_port
            );
            if(new_client_fd<0)
            {
                if(errno != EINTR)
                    perror("accept error");
            }
            else
            {
                if(client_fd>=0)
                {
                    printf("[TCP] replace previous client\n");
                    close(client_fd); 
                }                    
                client_fd = new_client_fd;
                receive_length = 0;            

                printf("[TCP] client connected: %s:%u\n",client_ip,(unsigned int)client_port);
                continue;
            }
 
        }
        //数据传输
        if(client_fd>=0)
        {
            short client_events = pollfd[1].revents;
            if((client_events&POLLIN)!=0)
            {
                char tmp_buffer[TEMP_BUFFER_SIZE];
                ssize_t received;

                received = recv(client_fd,tmp_buffer,sizeof(tmp_buffer),0);
                if(received>0)
                {
                    size_t received_size = (size_t)received;
                    //溢出后踢出此次连接，并重置接收长度，其实可以考虑扔掉这次数据而不断开连接
                    if(receive_length+received_size>sizeof(receive_buffer))
                    {
                        fprintf(stderr,"[TCP] accumulated receive buffer overflow\n");
                        close(client_fd);
                        client_fd =-1;
                        receive_length = 0;
                        continue;
                    }
                    //拼接消息放到receivebuffer当中
                    memcpy(receive_buffer+receive_length,tmp_buffer,received_size);
                    receive_length +=received_size;

                    line_parser_feed(
                        receive_buffer,
                        &receive_length,
                        sizeof(receive_buffer),
                        on_line,
                        &server_ctx
                    );
                }else if(received == 0)
                {
                    printf("[TCP] client disconnected\n");
                    close(client_fd);
                    client_fd = -1;
                    receive_length = 0;
                }
                else if(errno != EINTR){
                    perror("recv");
                    close(client_fd);
                    client_fd =-1;
                    receive_length =0;
                
                }
            }
            if (client_fd >= 0 &&
                (client_events &
                 (POLLERR | POLLHUP | POLLNVAL)) != 0)
            {
                printf(
                    "[TCP] client connection closed\n"
                );

                close(client_fd);
                client_fd = -1;
                receive_length = 0;
            }
        }
    }
    if (client_fd >= 0)
    {
        close(client_fd);
    }

    close(server_fd);

    command_manager_destroy(manager);
    node_store_destroy(store);

    printf("TCP server stopped.\n");

    return 0;
}