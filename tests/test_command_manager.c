#define _POSIX_C_SOURCE 200809L

/*
 * =====================================================================
 * command_manager 单元测试
 *
 * 重点覆盖 M7-7C4-1 / C4-3 引入的行为：
 *   命令必须记住 target_node，且超时重发时把它原样带回来（C4-1）；
 *   多条不同目标的命令同时超时时，各自的重发目标不能串（C4-3）；
 *   ACK/NACK 必须来自命令的原目标节点，否则不予采纳（C4-4）。
 *
 * 这是 Server 侧路由正确性的前提：
 *   缺失 target_node 时组出的 CMD JSON 是 {"node":"", ...}，
 *   网关会因为目标非法而丢弃，命令重试机制形同虚设；
 *   而重发时张冠李戴，则会把 NODE02 的命令打到 NODE01 上；
 *   应答不加身份校验，则别的节点的应答能冒充成功。
 * =====================================================================
 */

#include "command_manager.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define RETRY_TABLE 8

static void test_send_saves_target_node(void)
{
    command_manager_t *manager;
    frame_kv_t fields[1];
    uint32_t sequence = 0;
    command_state_t state = COMMAND_STATE_FREE;

    manager = command_manager_create();
    assert(manager != NULL);

    memset(fields, 0, sizeof(fields));
    strcpy(fields[0].key, "LED");
    strcpy(fields[0].value, "1");

    assert(
        command_manager_send(
            manager,
            "NODE01",
            "GATEWAY",
            fields,
            1U,
            &sequence
        ) == 0
    );

    assert(sequence != 0U);

    /* 命令应进入 WAITING，等待 ACK/NACK */
    assert(
        command_manager_query(manager, sequence, &state) == 0
    );

    assert(state == COMMAND_STATE_WAITING);

    /* 参数非法：target_node 为空必须被拒绝 */
    assert(
        command_manager_send(
            manager,
            "",
            "GATEWAY",
            fields,
            1U,
            &sequence
        ) == -1
    );

    assert(
        command_manager_send(
            manager,
            NULL,
            "GATEWAY",
            fields,
            1U,
            &sequence
        ) == -1
    );

    /* target_node 过长必须被拒绝，而不是截断 */
    {
        char too_long[COMMAND_TARGET_NODE_SIZE + 8];
        size_t i;

        for (i = 0; i < sizeof(too_long) - 1U; i++)
        {
            too_long[i] = 'N';
        }

        too_long[sizeof(too_long) - 1U] = '\0';

        assert(
            command_manager_send(
                manager,
                too_long,
                "GATEWAY",
                fields,
                1U,
                &sequence
            ) == -1
        );
    }

    command_manager_destroy(manager);
}

/*
 * C4-1 的核心用例：
 * 超时重发时必须把原来的 target_node 一起带出来。
 */
static void test_retry_keeps_target_node(void)
{
    command_manager_t *manager;
    frame_kv_t fields[1];
    uint32_t sequence = 0;

    uint32_t retry_sequences[RETRY_TABLE];
    char retry_target_nodes[RETRY_TABLE][COMMAND_TARGET_NODE_SIZE];
    frame_kv_t retry_fields[RETRY_TABLE][FRAME_DATA_MAX_FIELDS];
    size_t retry_field_counts[RETRY_TABLE];
    uint32_t timeout_sequences[RETRY_TABLE];

    size_t retry_count = 0;
    size_t timeout_count = 0;

    manager = command_manager_create();
    assert(manager != NULL);

    memset(fields, 0, sizeof(fields));
    strcpy(fields[0].key, "LED");
    strcpy(fields[0].value, "1");

    assert(
        command_manager_send(
            manager,
            "NODE02",
            "GATEWAY",
            fields,
            1U,
            &sequence
        ) == 0
    );

    /*
     * 用 now + 超时阈值 模拟"已经等够时间"，
     * 避免测试真的 sleep。
     */
    assert(
        command_manager_check_timeouts(
            manager,
            time(NULL) + 10,
            3,
            1,
            retry_sequences,
            retry_target_nodes,
            retry_fields,
            retry_field_counts,
            RETRY_TABLE,
            &retry_count,
            timeout_sequences,
            RETRY_TABLE,
            &timeout_count
        ) == 0
    );

    assert(retry_count == 1U);
    assert(timeout_count == 0U);

    assert(retry_sequences[0] == sequence);

    /*
     * 这里就是 C4-1 要保证的关键点：
     * 重发记录里必须还带着目标节点，
     * 否则重发组出的 JSON 会是 {"node":"", ...}。
     */
    assert(strcmp(retry_target_nodes[0], "NODE02") == 0);

    assert(strcmp(retry_fields[0][0].key, "LED") == 0);
    assert(strcmp(retry_fields[0][0].value, "1") == 0);
    assert(retry_field_counts[0] == 1U);

    /*
     * 再多超时一次：retries 已达 max_retries，
     * 这次应该转入 TIMEOUT 而不是继续重发。
     */
    assert(
        command_manager_check_timeouts(
            manager,
            time(NULL) + 20,
            3,
            1,
            retry_sequences,
            retry_target_nodes,
            retry_fields,
            retry_field_counts,
            RETRY_TABLE,
            &retry_count,
            timeout_sequences,
            RETRY_TABLE,
            &timeout_count
        ) == 0
    );

    assert(retry_count == 0U);
    assert(timeout_count == 1U);
    assert(timeout_sequences[0] == sequence);

    command_manager_destroy(manager);
}

/*
 * ACK 后不应再被当成超时命令重发。
 */
static void test_acked_command_is_not_retried(void)
{
    command_manager_t *manager;
    frame_kv_t fields[1];
    frame_ack_t ack;
    uint32_t sequence = 0;

    uint32_t retry_sequences[RETRY_TABLE];
    char retry_target_nodes[RETRY_TABLE][COMMAND_TARGET_NODE_SIZE];
    frame_kv_t retry_fields[RETRY_TABLE][FRAME_DATA_MAX_FIELDS];
    size_t retry_field_counts[RETRY_TABLE];
    uint32_t timeout_sequences[RETRY_TABLE];

    size_t retry_count = 0;
    size_t timeout_count = 0;

    manager = command_manager_create();
    assert(manager != NULL);

    memset(fields, 0, sizeof(fields));
    strcpy(fields[0].key, "LED");
    strcpy(fields[0].value, "0");

    assert(
        command_manager_send(
            manager,
            "NODE01",
            "GATEWAY",
            fields,
            1U,
            &sequence
        ) == 0
    );

    memset(&ack, 0, sizeof(ack));
    strcpy(ack.node_id, "NODE01");
    ack.sequence = sequence;

    assert(command_manager_on_ack(manager, &ack) == 0);

    assert(
        command_manager_check_timeouts(
            manager,
            time(NULL) + 30,
            3,
            2,
            retry_sequences,
            retry_target_nodes,
            retry_fields,
            retry_field_counts,
            RETRY_TABLE,
            &retry_count,
            timeout_sequences,
            RETRY_TABLE,
            &timeout_count
        ) == 0
    );

    assert(retry_count == 0U);
    assert(timeout_count == 0U);

    command_manager_destroy(manager);
}

/*
 * M7-7C4-3 的核心用例：
 * 多条不同目标节点的命令同时超时时，
 * 每条重发都必须带自己原来的目标，不能串到别的节点上。
 *
 * 这是"第一次路由正确、重试时串节点"这一风险的防线。
 */
static void test_multi_node_retry_isolated(void)
{
    command_manager_t *manager;

    frame_kv_t node01_fields[1];
    frame_kv_t node02_fields[1];

    uint32_t node01_sequence = 0;
    uint32_t node02_sequence = 0;

    uint32_t retry_sequences[RETRY_TABLE];
    char retry_target_nodes[RETRY_TABLE][COMMAND_TARGET_NODE_SIZE];
    frame_kv_t retry_fields[RETRY_TABLE][FRAME_DATA_MAX_FIELDS];
    size_t retry_field_counts[RETRY_TABLE];
    uint32_t timeout_sequences[RETRY_TABLE];

    size_t retry_count = 0;
    size_t timeout_count = 0;
    size_t i;

    int found_node01 = 0;
    int found_node02 = 0;

    manager = command_manager_create();
    assert(manager != NULL);

    /* NODE01: LED=1 */
    memset(node01_fields, 0, sizeof(node01_fields));
    strcpy(node01_fields[0].key, "LED");
    strcpy(node01_fields[0].value, "1");

    /* NODE02: LED=0 —— 值与 NODE01 不同，便于发现错配 */
    memset(node02_fields, 0, sizeof(node02_fields));
    strcpy(node02_fields[0].key, "LED");
    strcpy(node02_fields[0].value, "0");

    assert(
        command_manager_send(
            manager,
            "NODE01",
            "GATEWAY",
            node01_fields,
            1U,
            &node01_sequence
        ) == 0
    );

    assert(
        command_manager_send(
            manager,
            "NODE02",
            "GATEWAY",
            node02_fields,
            1U,
            &node02_sequence
        ) == 0
    );

    /* 两条命令必须拿到各自的序号 */
    assert(node01_sequence != node02_sequence);

    /*
     * 人为把时间推到未来，不需要真的 sleep 3 秒。
     */
    assert(
        command_manager_check_timeouts(
            manager,
            time(NULL) + 10,
            3,      /* timeout_sec */
            2,      /* max_retries */
            retry_sequences,
            retry_target_nodes,
            retry_fields,
            retry_field_counts,
            RETRY_TABLE,
            &retry_count,
            timeout_sequences,
            RETRY_TABLE,
            &timeout_count
        ) == 0
    );

    /* 两条都应触发第一次重发，且都还没到 TIMEOUT */
    assert(retry_count == 2U);
    assert(timeout_count == 0U);

    /*
     * 不依赖返回顺序：
     * 先按 sequence 找到对应项，再校验 target 与字段。
     */
    for (i = 0U; i < retry_count; i++)
    {
        if (retry_sequences[i] == node01_sequence)
        {
            assert(strcmp(retry_target_nodes[i], "NODE01") == 0);
            assert(retry_field_counts[i] == 1U);
            assert(strcmp(retry_fields[i][0].key, "LED") == 0);
            assert(strcmp(retry_fields[i][0].value, "1") == 0);

            found_node01 = 1;
        }
        else if (retry_sequences[i] == node02_sequence)
        {
            assert(strcmp(retry_target_nodes[i], "NODE02") == 0);
            assert(retry_field_counts[i] == 1U);
            assert(strcmp(retry_fields[i][0].key, "LED") == 0);
            assert(strcmp(retry_fields[i][0].value, "0") == 0);

            found_node02 = 1;
        }
    }

    /* 两条都必须被检查到，避免"只查了一条"的假通过 */
    assert(found_node01);
    assert(found_node02);

    command_manager_destroy(manager);
}

static void test_ack_target_validation(void)
{
    command_manager_t *manager;

    frame_kv_t fields[1];

    frame_ack_t ack;

    uint32_t sequence;

    command_state_t state;


    manager =
        command_manager_create();

    assert(
        manager != NULL
    );


    memset(
        fields,
        0,
        sizeof(fields)
    );

    strcpy(
        fields[0].key,
        "LED"
    );

    strcpy(
        fields[0].value,
        "1"
    );


    /*
     * 命令明确发给 NODE01。
     */
    assert(
        command_manager_send(
            manager,
            "NODE01",
            "GATEWAY",
            fields,
            1U,
            &sequence
        ) == 0
    );


    /*
     * NODE02伪造/错误返回同sequence ACK。
     */
    memset(
        &ack,
        0,
        sizeof(ack)
    );

    strcpy(
        ack.node_id,
        "NODE02"
    );

    ack.sequence =
        sequence;


    assert(
        command_manager_on_ack(
            manager,
            &ack
        ) == -1
    );


    /*
     * 状态必须仍然是 WAITING。
     */
    assert(
        command_manager_query(
            manager,
            sequence,
            &state
        ) == 0
    );

    assert(
        state ==
        COMMAND_STATE_WAITING
    );


    /*
     * 正确的 NODE01 ACK。
     */
    strcpy(
        ack.node_id,
        "NODE01"
    );


    assert(
        command_manager_on_ack(
            manager,
            &ack
        ) == 0
    );


    assert(
        command_manager_query(
            manager,
            sequence,
            &state
        ) == 0
    );

    assert(
        state ==
        COMMAND_STATE_ACKED
    );


    command_manager_destroy(
        manager
    );


    printf(
        "[PASS] ACK target validation\n"
    );
}

static void test_nack_target_validation(void)
{
    command_manager_t *manager;

    frame_kv_t fields[1];

    frame_nack_t nack;

    uint32_t sequence;

    command_state_t state;


    manager =
        command_manager_create();

    assert(
        manager != NULL
    );


    memset(
        fields,
        0,
        sizeof(fields)
    );

    strcpy(
        fields[0].key,
        "LED"
    );

    strcpy(
        fields[0].value,
        "0"
    );


    assert(
        command_manager_send(
            manager,
            "NODE02",
            "GATEWAY",
            fields,
            1U,
            &sequence
        ) == 0
    );


    /*
     * 错误：NODE01返回NODE02命令的NACK。
     */
    memset(
        &nack,
        0,
        sizeof(nack)
    );

    strcpy(
        nack.node_id,
        "NODE01"
    );

    nack.sequence =
        sequence;

    strcpy(
        nack.error,
        "wrong_node"
    );


    assert(
        command_manager_on_nack(
            manager,
            &nack
        ) == -1
    );


    assert(
        command_manager_query(
            manager,
            sequence,
            &state
        ) == 0
    );

    assert(
        state ==
        COMMAND_STATE_WAITING
    );


    /*
     * 正确：NODE02返回NACK。
     */
    strcpy(
        nack.node_id,
        "NODE02"
    );


    assert(
        command_manager_on_nack(
            manager,
            &nack
        ) == 0
    );


    assert(
        command_manager_query(
            manager,
            sequence,
            &state
        ) == 0
    );

    assert(
        state ==
        COMMAND_STATE_NACKED
    );


    command_manager_destroy(
        manager
    );


    printf(
        "[PASS] NACK target validation\n"
    );
}


int main(void)
{
    printf("Running command_manager tests...\n");

    test_send_saves_target_node();
    printf("  test_send_saves_target_node    PASS\n");

    test_retry_keeps_target_node();
    printf("  test_retry_keeps_target_node   PASS\n");

    test_acked_command_is_not_retried();
    printf("  test_acked_command_is_not_retried PASS\n");

    test_multi_node_retry_isolated();
    printf("  test_multi_node_retry_isolated PASS\n");

    test_ack_target_validation();
    printf("  test_ack_target_validation PASS\n");

    test_nack_target_validation();
    printf("  test_nack_target_validation PASS\n");

    printf("All command_manager tests passed.\n");

    return 0;
}
