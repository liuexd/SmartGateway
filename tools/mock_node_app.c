#define _POSIX_C_SOURCE 200809L

#include "mock_node_app.h"

#include "frame.h"
#include "serial_port.h"

#include <errno.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/*
 * write()不保证一次写完全部数据，
 * 所以需要循环写入。
 */
static int write_all(
    int fd,
    const void *buffer,
    size_t length
)
{
    const uint8_t *data = (const uint8_t *)buffer;
    size_t total = 0;

    while (total < length)
    {
        ssize_t written = write(
            fd,
            data + total,
            length - total
        );

        if (written > 0)
        {
            total += (size_t)written;
            continue;
        }

        if (written < 0 && errno == EINTR)
        {
            continue;
        }

        perror("write");
        return -1;
    }

    return 0;
}

/*
 * 节点回调上下文：
 * 保存串口文件描述符与模拟设备状态，
 * 供回调中"执行操作"和回发ACK/NACK使用。
 */
typedef struct
{
    int fd;

    /*
     * 模拟设备当前LED状态：0=灭，1=亮。
     * 收到合法CMD时更新，ACK中回显。
     */
    int led_state;

    /*
     * 偶发故障模拟：
     * fail_every  每N条合法命令模拟一次故障（0=从不）
     * fail_counter 已处理的合法命令数
     */
    int fail_every;
    int fail_counter;
} node_context_t;

/*
 * 每成功解析出一帧，frame_parser_feed() 会调用该回调。
 *
 * 节点只关心网关下发的 CMD 命令帧：
 *   1. 用 frame_get_message_type() 判断帧类型；
 *   2. CMD 帧解析失败 -> 回发 NACK 说明原因（bad_cmd）；
 *   3. CMD 帧解析成功但参数非法（led 不是0/1）
 *      -> 模拟设备操作失败，回发 NACK（invalid_led）；
 *   4. CMD 帧解析成功且参数合法：
 *      - 命中偶发故障（fail_every）-> 模拟硬件故障，回发 NACK（sim_fault）；
 *      - 否则模拟设备执行操作成功，更新LED状态，回发 ACK 确认；
 *   5. 其它类型帧（DATA/ACK/NACK）一律忽略。
 */
static void on_node_frame(
    const parsed_frame_t *frame,
    void *user_data)
{
    node_context_t *context;
    frame_message_type_t type;
    char reply[FRAME_MAX_LEN];
    int reply_length;

    if (frame == NULL || user_data == NULL)
    {
        return;
    }

    context = (node_context_t *)user_data;

    type = frame_get_message_type(frame);

    switch (type)
    {
        case FRAME_MESSAGE_CMD:
        {
            frame_command_t cmd;
            const char *led_text;
            int led_value;

            if (frame_decode_command(frame, &cmd) == 0)
            {
                /*
                 * 解析成功：从通用键值对中取 LED 参数。
                 *
                 * 节点按协议字典解释字段：
                 * 这里只处理 LED（0=灭，1=亮），
                 * 其它未知字段忽略（未知命令/参数视为不支持）。
                 */
                led_text = frame_fields_find(
                    cmd.fields,
                    cmd.field_count,
                    "LED"
                );

                if (led_text == NULL)
                {
                    /*
                     * 命令里没有LED参数：
                     * 设备不支持该命令，回发NACK。
                     */
                    printf(
                        "[NODE] CMD from=%s seq=%06u no LED field "
                        "-> unsupported, NACK\n",
                        cmd.sender,
                        (unsigned int)cmd.sequence
                    );

                    reply_length = frame_build_nack(
                        reply,
                        sizeof(reply),
                        "NODE01",
                        cmd.sequence,
                        "unsupported_cmd"
                    );
                    break;
                }

                led_value = atoi(led_text);

                /*
                 * LED控制值必须是0或1；
                 * 其它值视为设备不支持该操作，操作失败。
                 */
                if (led_value != 0 && led_value != 1)
                {
                    printf(
                        "[NODE] CMD from=%s seq=%06u LED=%s "
                        "-> unsupported, NACK\n",
                        cmd.sender,
                        (unsigned int)cmd.sequence,
                        led_text
                    );

                    reply_length = frame_build_nack(
                        reply,
                        sizeof(reply),
                        "NODE01",
                        cmd.sequence,
                        "invalid_led"
                    );
                }
                else
                {
                    /*
                     * 参数合法：先计数，再判断是否命中偶发故障。
                     */
                    context->fail_counter++;

                    if (context->fail_every > 0 &&
                        context->fail_counter %
                            context->fail_every == 0)
                    {
                        /*
                         * 模拟设备偶发硬件故障，回发NACK。
                         */
                        printf(
                            "[NODE] CMD from=%s seq=%06u LED=%s "
                            "-> simulated fault, NACK\n",
                            cmd.sender,
                            (unsigned int)cmd.sequence,
                            led_text
                        );

                        reply_length = frame_build_nack(
                            reply,
                            sizeof(reply),
                            "NODE01",
                            cmd.sequence,
                            "sim_fault"
                        );
                    }
                    else
                    {
                        /*
                         * 操作成功：更新设备LED状态，回发ACK确认。
                         *
                         * ACK 使用通用键值对回显设备状态，
                         * 与 DATA/CMD 保持同一套 KV 机制。
                         */
                        frame_kv_t ack_fields[1];

                        context->led_state = led_value;

                        snprintf(
                            ack_fields[0].key,
                            sizeof(ack_fields[0].key),
                            "LED"
                        );
                        snprintf(
                            ack_fields[0].value,
                            sizeof(ack_fields[0].value),
                            "%d",
                            context->led_state
                        );

                        printf(
                            "[NODE] CMD from=%s seq=%06u LED=%s "
                            "-> OK, led_state=%d, ACK\n",
                            cmd.sender,
                            (unsigned int)cmd.sequence,
                            led_text,
                            context->led_state
                        );

                        reply_length = frame_build_ack_kv(
                            reply,
                            sizeof(reply),
                            "NODE01",
                            cmd.sequence,
                            ack_fields,
                            1U
                        );
                    }
                }
            }
            else
            {
                /*
                 * 解析失败：回发 NACK，错误原因bad_cmd。
                 * 此时无法取到可靠序号，sequence填0。
                 */
                fprintf(
                    stderr,
                    "[NODE] CMD decode failed: %s",
                    frame->raw
                );

                reply_length = frame_build_nack(
                    reply,
                    sizeof(reply),
                    "NODE01",
                    0,
                    "bad_cmd"
                );
            }
            break;
        }

        case FRAME_MESSAGE_DATA:
        case FRAME_MESSAGE_ACK:
        case FRAME_MESSAGE_NACK:
        case FRAME_MESSAGE_UNKNOWN:
        default:
            /*
             * 节点只响应命令，其它帧忽略。
             */
            return;
    }

    if (reply_length < 0)
    {
        fprintf(
            stderr,
            "[NODE] build reply failed: %d\n",
            reply_length
        );
        return;
    }

    if (write_all(
            context->fd,
            reply,
            (size_t)reply_length
        ) != 0)
    {
        fprintf(stderr, "[NODE] send reply failed\n");
        return;
    }

    printf("[NODE] TX %s", reply);
}

void mock_node_app_init(mock_node_app_t *app, int fd)
{
    if (app == NULL)
    {
        return;
    }

    app->fd = fd;
    app->fail_every = 0;
}

int mock_node_app_run(
    mock_node_app_t *app,
    const volatile int *running
)
{
    char frame[FRAME_MAX_LEN];
    uint8_t read_buffer[4096];
    frame_parser_t parser;
    node_context_t node_context;
    uint32_t sequence = 0;
    time_t next_report;
    int temperature_x10;
    int humidity_x10;
    int frame_length;
    int wait_result;

    if (app == NULL || running == NULL)
    {
        return -1;
    }

    /*
     * 解析器在循环外创建并初始化：
     * 这样一帧被拆成多次read时，跨循环仍能保留半帧状态。
     */
    frame_parser_init(&parser);

    node_context.fd = app->fd;
    node_context.led_state = 0;
    node_context.fail_every = app->fail_every;
    node_context.fail_counter = 0;

    /*
     * 上报节流时间点：达到该时间才发一帧DATA。
     *
     * 不用sleep(1)的原因：
     * 如果每轮上报后无条件睡1秒，命令到达时节点无法及时响应。
     * 改为高频轮询串口（100ms超时），
     * 命令响应延迟最多100ms，而DATA仍保持约每秒一帧。
     */
    next_report = time(NULL);

    while (*running)
    {
        time_t now;

        wait_result = serial_port_wait_readable(
            app->fd,
            100
        );

        if (wait_result == SERIAL_WAIT_READABLE)
        {
            ssize_t length = serial_port_read(
                app->fd,
                read_buffer,
                sizeof(read_buffer)
            );

            if (length > 0)
            {
                frame_parser_feed(
                    &parser,
                    read_buffer,
                    (size_t)length,
                    on_node_frame,
                    &node_context
                );
            }
        }

        now = time(NULL);

        if (now < next_report)
        {
            /*
             * 还没到上报时间点：
             * 跳过本轮上报，立即回到循环继续监听串口，
             * 保证命令能被及时处理。
             */
            continue;
        }

        /*
         * 到达上报时间点：构造并上报一帧数据。
         *
         * 使用通用 KV 接口 frame_build_data_kv()：
         *   - DEV=1 标识设备类型（温湿度传感器，见协议字典）；
         *   - T/H/P 为传感器数据；
         * 演示同一套协议承载任意字段、任意设备类型。
         */
        frame_kv_t fields[4];

        temperature_x10 = 250 + (int)(sequence % 10U);
        humidity_x10 = 500 + (int)(sequence % 20U);

        snprintf(fields[0].key, sizeof(fields[0].key), FRAME_KV_DEV);
        snprintf(fields[0].value, sizeof(fields[0].value), "%d",
                 FRAME_DEVICE_THSENSOR);
        snprintf(fields[1].key, sizeof(fields[1].key), "T");
        snprintf(fields[1].value, sizeof(fields[1].value), "%d", temperature_x10);
        snprintf(fields[2].key, sizeof(fields[2].key), "H");
        snprintf(fields[2].value, sizeof(fields[2].value), "%d", humidity_x10);
        snprintf(fields[3].key, sizeof(fields[3].key), "P");
        snprintf(fields[3].value, sizeof(fields[3].value), "%d", 1013 + (int)(sequence % 10U));

        frame_length = frame_build_data_kv(
            frame,
            sizeof(frame),
            "NODE01",
            sequence,
            fields,
            4U
        );

        if (frame_length < 0)
        {
            fprintf(
                stderr,
                "frame_build_data_kv failed: %d\n",
                frame_length
            );

            return 1;
        }

        if (write_all(
                app->fd,
                frame,
                (size_t)frame_length
            ) != 0)
        {
            return 1;
        }

        printf(
            "TX seq=%06u, dev=%s, fields=T=%d,H=%d,P=%d\n",
            (unsigned int)sequence,
            frame_device_type_name(FRAME_DEVICE_THSENSOR),
            temperature_x10,
            humidity_x10,
            1013 + (int)(sequence % 10U)
        );

        /*
         * 序号溢出后回到0。
         */
        sequence = (sequence + 1U) % 1000000U;

        /*
         * 下一次上报时间：约1秒后。
         */
        next_report = now + 5;
    }

    return 0;
}
