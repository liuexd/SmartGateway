/*
 * =====================================================================
 * 命令管理器（command_manager）
 *
 * 数据结构：定长数组 + 线性查找。
 *
 * 选型理由：
 *   - 并发在途命令通常很少（几个到几十个），线性查找 O(n) 足够；
 *   - 定长数组内存紧凑、实现简单，无需动态分配。
 *
 * 序号分配：
 *   next_sequence 单调递增并循环（溢出回绕），
 *   保证 WAITING 中的命令序号不冲突（槽位空闲时序号早已复用）。
 * =====================================================================
 */

#include "command_manager.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * 命令跟踪表容量。
 *
 * 同时允许最多 64 条在途命令，超出后 send() 返回失败。
 */
#define COMMAND_MANAGER_MAX_COMMANDS 64

/*
 * 单个命令条目。
 *
 * state     ：当前状态（见 command_state_t）
 * sequence  ：命令序号，与ACK/NACK应答配对
 * sender    ：发送方标识
 * led_value ：命令参数
 * sent_time ：最近一次下发时间（超时判断用）
 * retries   ：已重试次数
 */
typedef struct
{
    command_state_t state;
    uint32_t sequence;
    char sender[32];
    int led_value;
    time_t sent_time;
    int retries;
} command_entry_t;

/*
 * 命令管理器主体。
 *
 * entries      ：定长跟踪表
 * next_sequence：下一个要分配的命令序号
 */
struct command_manager
{
    command_entry_t entries[COMMAND_MANAGER_MAX_COMMANDS];
    uint32_t next_sequence;
};

command_manager_t *command_manager_create(void)
{
    command_manager_t *manager;

    manager = (command_manager_t *)calloc(
        1,
        sizeof(*manager)
    );

    if (manager == NULL)
    {
        return NULL;
    }

    /*
     * 序号从1开始，0保留给"未知序号"（如解析失败时的NACK）。
     */
    manager->next_sequence = 1;

    return manager;
}

void command_manager_destroy(command_manager_t *manager)
{
    free(manager);
}

int command_manager_build_cmd(
    char *out,
    size_t out_size,
    const char *sender,
    uint32_t sequence,
    int led_value
)
{
    return frame_build_command(
        out,
        out_size,
        sender,
        sequence,
        led_value
    );
}

/*
 * 查找序号对应的条目。
 *
 * @return 找到返回条目指针；未找到返回NULL
 */
static command_entry_t *command_manager_find(
    command_manager_t *manager,
    uint32_t sequence)
{
    size_t i;

    if (manager == NULL)
    {
        return NULL;
    }

    for (i = 0; i < COMMAND_MANAGER_MAX_COMMANDS; i++)
    {
        if (manager->entries[i].state != COMMAND_STATE_FREE &&
            manager->entries[i].sequence == sequence)
        {
            return &manager->entries[i];
        }
    }

    return NULL;
}

int command_manager_send(
    command_manager_t *manager,
    const char *sender,
    int led_value,
    uint32_t *sequence_out)
{
    command_entry_t *entry;
    size_t i;

    if (manager == NULL ||
        sender == NULL ||
        sender[0] == '\0' ||
        sequence_out == NULL)
    {
        return -1;
    }

    /*
     * 找一个空闲槽位。
     */
    entry = NULL;

    for (i = 0; i < COMMAND_MANAGER_MAX_COMMANDS; i++)
    {
        if (manager->entries[i].state == COMMAND_STATE_FREE)
        {
            entry = &manager->entries[i];
            break;
        }
    }

    if (entry == NULL)
    {
        /*
         * 表满：在途命令太多。
         */
        return -1;
    }

    if (strlen(sender) >= sizeof(entry->sender))
    {
        return -1;
    }

    strcpy(entry->sender, sender);
    entry->sequence = manager->next_sequence;
    entry->led_value = led_value;
    entry->state = COMMAND_STATE_WAITING;
    entry->sent_time = time(NULL);
    entry->retries = 0;

    *sequence_out = entry->sequence;

    /*
     * 序号循环递增；0保留给"未知序号"。
     */
    manager->next_sequence++;

    if (manager->next_sequence == 0U)
    {
        manager->next_sequence = 1;
    }

    return 0;
}

int command_manager_on_ack(
    command_manager_t *manager,
    const frame_ack_t *ack)
{
    command_entry_t *entry;

    if (manager == NULL || ack == NULL)
    {
        return -1;
    }

    entry = command_manager_find(manager, ack->sequence);

    if (entry == NULL)
    {
        /*
         * 找不到对应命令：
         * 可能是设备重启后的旧应答，或序号已被复用。
         */
        return -1;
    }

    if (entry->state != COMMAND_STATE_WAITING)
    {
        /*
         * 命令已不是等待状态（例如已超时）：
         * 迟到的应答不改变状态，但视为找到。
         */
        return 0;
    }

    entry->state = COMMAND_STATE_ACKED;

    return 0;
}

int command_manager_on_nack(
    command_manager_t *manager,
    const frame_nack_t *nack)
{
    command_entry_t *entry;

    if (manager == NULL || nack == NULL)
    {
        return -1;
    }

    entry = command_manager_find(manager, nack->sequence);

    if (entry == NULL)
    {
        return -1;
    }

    if (entry->state != COMMAND_STATE_WAITING)
    {
        return 0;
    }

    entry->state = COMMAND_STATE_NACKED;

    return 0;
}

int command_manager_query(
    const command_manager_t *manager,
    uint32_t sequence,
    command_state_t *state_out)
{
    const command_entry_t *entry;
    size_t i;

    if (manager == NULL || state_out == NULL)
    {
        return -1;
    }

    for (i = 0; i < COMMAND_MANAGER_MAX_COMMANDS; i++)
    {
        entry = &manager->entries[i];

        if (entry->state != COMMAND_STATE_FREE &&
            entry->sequence == sequence)
        {
            *state_out = entry->state;
            return 0;
        }
    }

    return -1;
}

int command_manager_check_timeouts(
    command_manager_t *manager,
    time_t now,
    int timeout_sec,
    int max_retries,
    uint32_t *retry_sequences,
    int *retry_leds,
    size_t retry_capacity,
    size_t *retry_count,
    uint32_t *timeout_sequences,
    size_t timeout_capacity,
    size_t *timeout_count)
{
    size_t i;
    size_t retries = 0;
    size_t timed_out = 0;

    if (manager == NULL ||
        timeout_sec < 0 ||
        max_retries < 0)
    {
        return -1;
    }

    if (retry_count != NULL)
    {
        *retry_count = 0;
    }

    if (timeout_count != NULL)
    {
        *timeout_count = 0;
    }

    for (i = 0; i < COMMAND_MANAGER_MAX_COMMANDS; i++)
    {
        command_entry_t *entry = &manager->entries[i];

        if (entry->state != COMMAND_STATE_WAITING)
        {
            continue;
        }

        if (now - entry->sent_time < timeout_sec)
        {
            continue;
        }

        /*
         * 超时了。
         */
        if (entry->retries < max_retries)
        {
            /*
             * 还有重试机会：标记重发，状态保持WAITING。
             */
            entry->retries++;
            entry->sent_time = now;

            if (retry_sequences != NULL)
            {
                /*
                 * 只有真正写进数组才计数，
                 * 保证 retry_count <= retry_capacity，
                 * 调用方按 retry_count 遍历不会越界。
                 */
                if (retries < retry_capacity)
                {
                    retry_sequences[retries] = entry->sequence;

                    if (retry_leds != NULL)
                    {
                        retry_leds[retries] = entry->led_value;
                    }

                    retries++;
                }
                /*
                 * 数组已满：该条的重发记录被丢弃，
                 * 调用方容量有限，无法处理更多。
                 */
            }
            else
            {
                /*
                 * 调用方不需要序号列表，只统计数量。
                 */
                retries++;
            }
        }
        else
        {
            /*
             * 重试耗尽：标记超时失败。
             */
            entry->state = COMMAND_STATE_TIMEOUT;

            if (timeout_sequences != NULL)
            {
                /*
                 * 同 retry_sequences：只有写进数组才计数。
                 */
                if (timed_out < timeout_capacity)
                {
                    timeout_sequences[timed_out] = entry->sequence;
                    timed_out++;
                }
            }
            else
            {
                timed_out++;
            }
        }
    }

    if (retry_count != NULL)
    {
        *retry_count = retries;
    }

    if (timeout_count != NULL)
    {
        *timeout_count = timed_out;
    }

    return 0;
}
