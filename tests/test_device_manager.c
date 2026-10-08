#define _POSIX_C_SOURCE 200809L

#include "device_manager.h"

#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>


static void test_register_and_find(void)
{
    device_manager_t manager;
    device_t device;

    pthread_t self =
        pthread_self();

    assert(
        device_manager_init(&manager) ==
        DEVICE_MANAGER_OK
    );

    assert(
        device_manager_register(
            &manager,
            "NODE01",
            DEVICE_TRANSPORT_BLUETOOTH,
            10,
            self
        ) == DEVICE_MANAGER_OK
    );

    assert(
        manager.count == 1U
    );

    assert(
        device_manager_find(
            &manager,
            "NODE01",
            &device
        ) == DEVICE_MANAGER_OK
    );

    assert(
        strcmp(
            device.node_id,
            "NODE01"
        ) == 0
    );

    assert(
        device.transport ==
        DEVICE_TRANSPORT_BLUETOOTH
    );

    assert(
        device.fd == 10
    );

    assert(
        device.online == 1
    );

    assert(
        pthread_equal(
            device.thread_id,
            self
        ) != 0
    );

    device_manager_destroy(
        &manager
    );

    printf(
        "[PASS] register and find\n"
    );
}


static void test_duplicate_register(void)
{
    device_manager_t manager;

    assert(
        device_manager_init(&manager) ==
        DEVICE_MANAGER_OK
    );

    assert(
        device_manager_register(
            &manager,
            "NODE02",
            DEVICE_TRANSPORT_WIFI,
            20,
            pthread_self()
        ) == DEVICE_MANAGER_OK
    );

    assert(
        device_manager_register(
            &manager,
            "NODE02",
            DEVICE_TRANSPORT_WIFI,
            21,
            pthread_self()
        ) == DEVICE_MANAGER_EXISTS
    );

    assert(
        manager.count == 1U
    );

    device_manager_destroy(
        &manager
    );

    printf(
        "[PASS] duplicate register\n"
    );
}


static void test_online_offline(void)
{
    device_manager_t manager;
    device_t device;

    assert(
        device_manager_init(&manager) ==
        DEVICE_MANAGER_OK
    );

    assert(
        device_manager_register(
            &manager,
            "NODE02",
            DEVICE_TRANSPORT_WIFI,
            20,
            pthread_self()
        ) == DEVICE_MANAGER_OK
    );

    assert(
        device_manager_set_online(
            &manager,
            "NODE02",
            0,
            -1
        ) == DEVICE_MANAGER_OK
    );

    assert(
        device_manager_find(
            &manager,
            "NODE02",
            &device
        ) == DEVICE_MANAGER_OK
    );

    assert(device.online == 0);
    assert(device.fd == -1);

    assert(
        device_manager_set_online(
            &manager,
            "NODE02",
            1,
            25
        ) == DEVICE_MANAGER_OK
    );

    assert(
        device_manager_find(
            &manager,
            "NODE02",
            &device
        ) == DEVICE_MANAGER_OK
    );

    assert(device.online == 1);
    assert(device.fd == 25);

    device_manager_destroy(
        &manager
    );

    printf(
        "[PASS] online/offline\n"
    );
}


static void test_unregister(void)
{
    device_manager_t manager;
    device_t device;

    assert(
        device_manager_init(&manager) ==
        DEVICE_MANAGER_OK
    );

    assert(
        device_manager_register(
            &manager,
            "NODE01",
            DEVICE_TRANSPORT_BLUETOOTH,
            10,
            pthread_self()
        ) == DEVICE_MANAGER_OK
    );

    assert(
        device_manager_register(
            &manager,
            "NODE02",
            DEVICE_TRANSPORT_WIFI,
            20,
            pthread_self()
        ) == DEVICE_MANAGER_OK
    );

    assert(
        manager.count == 2U
    );

    assert(
        device_manager_unregister(
            &manager,
            "NODE01"
        ) == DEVICE_MANAGER_OK
    );

    assert(
        manager.count == 1U
    );

    assert(
        device_manager_find(
            &manager,
            "NODE01",
            &device
        ) == DEVICE_MANAGER_NOT_FOUND
    );

    /*
     * NODE02应该已经被前移到数组0号位置，
     * find仍必须正常。
     */
    assert(
        device_manager_find(
            &manager,
            "NODE02",
            &device
        ) == DEVICE_MANAGER_OK
    );

    assert(
        strcmp(
            device.node_id,
            "NODE02"
        ) == 0
    );

    device_manager_destroy(
        &manager
    );

    printf(
        "[PASS] unregister\n"
    );
}


static void test_last_seen(void)
{
    device_manager_t manager;
    device_t device;

    assert(
        device_manager_init(&manager) ==
        DEVICE_MANAGER_OK
    );

    assert(
        device_manager_register(
            &manager,
            "NODE03",
            DEVICE_TRANSPORT_WIFI,
            30,
            pthread_self()
        ) == DEVICE_MANAGER_OK
    );

    /*
     * 手动把 last_seen 改成一个明显过时的时间。
     *
     * 这样 update_last_seen() 之后必须严格变大，
     * 才能证明它确实刷新了时间；
     * 如果只断言 ">= time(NULL)"，
     * 那么 register 时设的值也会满足，
     * 即使 update_last_seen() 什么都没做也测不出来。
     */
    manager.devices[0].last_seen = 1000;

    assert(
        device_manager_update_last_seen(
            &manager,
            "NODE03"
        ) == DEVICE_MANAGER_OK
    );

    assert(
        device_manager_find(
            &manager,
            "NODE03",
            &device
        ) == DEVICE_MANAGER_OK
    );

    assert(
        device.last_seen > 1000
    );

    /*
     * 不存在的节点必须返回 NOT_FOUND，
     * 且不能污染其他设备。
     */
    assert(
        device_manager_update_last_seen(
            &manager,
            "NODE99"
        ) == DEVICE_MANAGER_NOT_FOUND
    );

    device_manager_destroy(
        &manager
    );

    printf(
        "[PASS] last seen\n"
    );
}


/*
 *测试注册表容量上限。
 *
 *填满 DEVICE_MANAGER_MAX_DEVICES 个设备以后，
 *再注册必须返回 DEVICE_MANAGER_FULL，
 *且 count 不能超过上限。
 */
static void test_capacity_full(void)
{
    device_manager_t manager;
    char node_id[DEVICE_NODE_ID_SIZE];
    size_t i;

    assert(
        device_manager_init(&manager) ==
        DEVICE_MANAGER_OK
    );

    /*
     * 依次填满整个注册表。
     * 生成 "N0" + 两位序号，保证不超长且互不相同。
     */
    for(i = 0; i < DEVICE_MANAGER_MAX_DEVICES; i++)
    {
        snprintf(
            node_id,
            sizeof(node_id),
            "N%02zu",
            i
        );

        assert(
            device_manager_register(
                &manager,
                node_id,
                DEVICE_TRANSPORT_WIFI,
                (int)(100 + i),
                pthread_self()
            ) == DEVICE_MANAGER_OK
        );
    }

    assert(
        manager.count ==
        DEVICE_MANAGER_MAX_DEVICES
    );

    /*
     * 第 MAX+1 个必须被拒绝。
     * 注意用全新的 node_id，
     * 避免命中 EXISTS 分支而不是 FULL。
     */
    assert(
        device_manager_register(
            &manager,
            "OVERFLOW",
            DEVICE_TRANSPORT_WIFI,
            999,
            pthread_self()
        ) == DEVICE_MANAGER_FULL
    );

    assert(
        manager.count ==
        DEVICE_MANAGER_MAX_DEVICES
    );

    /*
     * 被拒绝的设备不应留下痕迹。
     */
    {
        device_t device;
        assert(
            device_manager_find(
                &manager,
                "OVERFLOW",
                &device
            ) == DEVICE_MANAGER_NOT_FOUND
        );
    }

    /*
     * 腾出一个位置以后必须能继续注册。
     * 这验证 FULL 只是容量限制，不是永久锁死。
     */
    assert(
        device_manager_unregister(
            &manager,
            "N00"
        ) == DEVICE_MANAGER_OK
    );

    assert(
        device_manager_register(
            &manager,
            "OVERFLOW",
            DEVICE_TRANSPORT_WIFI,
            999,
            pthread_self()
        ) == DEVICE_MANAGER_OK
    );

    device_manager_destroy(
        &manager
    );

    printf(
        "[PASS] capacity full\n"
    );
}


/*
 *并发测试用的 worker 参数。
 *
 *每个线程负责一批互不相同的 node_id，
 *在自己的编号区间内反复注册/查找/注销，
 *以触发多线程对同一张表的读写。
 */
typedef struct
{
    device_manager_t *manager;

    int worker_index;      /* 本线程编号，用于生成唯一的 node_id */

    int iterations;        /* 每轮注册多少个设备 */

    int failed;            /* 非 0 表示线程内断言失败（用普通 int，
                              测试失败时进程会 abort，这里主要用于排查） */

} concurrent_arg_t;

#define CONCURRENT_WORKERS     4
#define CONCURRENT_ITERATIONS  40

static void *concurrent_worker(void *arg)
{
    concurrent_arg_t *ctx;
    char node_id[DEVICE_NODE_ID_SIZE];
    int i;

    ctx = (concurrent_arg_t *)arg;

    for(i = 0; i < ctx->iterations; i++)
    {
        /*
         * 生成全局唯一的 node_id：
         * 用 worker_index 做前缀，避免不同线程注册同名设备，
         * 否则会命中 EXISTS 分支而不是真正测试并发插入。
         */
        snprintf(
            node_id,
            sizeof(node_id),
            "W%dN%03d",
            ctx->worker_index,
            i
        );

        /*
         * 注册 -> 查找 -> 注销 三个操作，
         * 每一步都会进入 device_manager 的临界区。
         * 多线程交错执行时，如果内部没锁，
         * TSan 会直接报数据竞争。
         */
        if(device_manager_register(
                ctx->manager,
                node_id,
                DEVICE_TRANSPORT_WIFI,
                1000 + ctx->worker_index,
                pthread_self()
            ) != DEVICE_MANAGER_OK)
        {
            ctx->failed = 1;
            continue;
        }

        {
            device_t device;

            if(device_manager_find(
                    ctx->manager,
                    node_id,
                    &device
                ) != DEVICE_MANAGER_OK)
            {
                ctx->failed = 1;
            }
            else if(strcmp(device.node_id, node_id) != 0)
            {
                ctx->failed = 1;
            }
        }

        /*
         * 主动注销，让注册表反复经历插入和删除，
         * 从而覆盖 unregister 里的搬移逻辑。
         */
        if(device_manager_unregister(
                ctx->manager,
                node_id
            ) != DEVICE_MANAGER_OK)
        {
            ctx->failed = 1;
        }
    }

    return NULL;
}


/*
 *多线程并发访问测试。
 *
 *这是本模块最核心的验证：
 *device_manager 会被 bluetooth_worker、
 *多个 wifi_client_worker、server_link_worker 并发调用，
 *因此必须证明内部 mutex 真的起作用。
 *
 *单线程测试对锁的正确性是"盲"的，
 *这里用 4 个线程同时做注册/查找/注销，
 *配合 make test-tsan 可检出数据竞争。
 */
static void test_concurrent_access(void)
{
    device_manager_t manager;
    pthread_t threads[CONCURRENT_WORKERS];
    concurrent_arg_t args[CONCURRENT_WORKERS];
    int i;

    assert(
        device_manager_init(&manager) ==
        DEVICE_MANAGER_OK
    );

    for(i = 0; i < CONCURRENT_WORKERS; i++)
    {
        args[i].manager = &manager;
        args[i].worker_index = i;
        args[i].iterations = CONCURRENT_ITERATIONS;
        args[i].failed = 0;

        assert(
            pthread_create(
                &threads[i],
                NULL,
                concurrent_worker,
                &args[i]
            ) == 0
        );
    }

    for(i = 0; i < CONCURRENT_WORKERS; i++)
    {
        assert(
            pthread_join(
                threads[i],
                NULL
            ) == 0
        );
    }

    /*
     * 每个线程收尾时都注销了自己的设备，
     * 因此注册表应回到空状态。
     */
    assert(manager.count == 0U);

    for(i = 0; i < CONCURRENT_WORKERS; i++)
    {
        assert(args[i].failed == 0);
    }

    device_manager_destroy(
        &manager
    );

    printf(
        "[PASS] concurrent access\n"
    );
}

static void test_reconnect(void)
{
    device_manager_t manager;
    device_t device;

    pthread_t first_thread;
    pthread_t second_thread;

    /*
     * 测试中直接用当前线程ID即可。
     *
     * 为了真正测试thread变化，
     * 后面并发测试还可以使用两个实际worker。
     */
    first_thread =
        pthread_self();

    second_thread =
        pthread_self();

    assert(
        device_manager_init(
            &manager
        ) == DEVICE_MANAGER_OK
    );


    /*
     * 第一次连接。
     */
    assert(
        device_manager_bind_connection(
            &manager,
            "NODE02",
            DEVICE_TRANSPORT_WIFI,
            10,
            first_thread
        ) == DEVICE_MANAGER_OK
    );

    assert(
        manager.count == 1U
    );


    assert(
        device_manager_disconnect(
            &manager,
            "NODE02",
            10,
            first_thread
        ) == DEVICE_MANAGER_OK
    );


    assert(
        device_manager_find(
            &manager,
            "NODE02",
            &device
        ) == DEVICE_MANAGER_OK
    );

    assert(
        device.online == 0
    );

    assert(
        device.fd == -1
    );


    /*
     * NODE02重新连接，新fd=20。
     */
    assert(
        device_manager_bind_connection(
            &manager,
            "NODE02",
            DEVICE_TRANSPORT_WIFI,
            20,
            second_thread
        ) == DEVICE_MANAGER_OK
    );


    /*
     * 不能产生第二条NODE02记录。
     */
    assert(
        manager.count == 1U
    );


    assert(
        device_manager_find(
            &manager,
            "NODE02",
            &device
        ) == DEVICE_MANAGER_OK
    );

    assert(
        device.online == 1
    );

    assert(
        device.fd == 20
    );


    device_manager_destroy(
        &manager
    );

    printf(
        "[PASS] reconnect\n"
    );
}

static void test_stale_disconnect(void)
{
    device_manager_t manager;
    device_t device;

    pthread_t self =
        pthread_self();

    assert(
        device_manager_init(
            &manager
        ) == DEVICE_MANAGER_OK
    );


    /*
     * 旧连接。
     */
    assert(
        device_manager_bind_connection(
            &manager,
            "NODE02",
            DEVICE_TRANSPORT_WIFI,
            10,
            self
        ) == DEVICE_MANAGER_OK
    );


    /*
     * 新连接已经接管。
     */
    assert(
        device_manager_bind_connection(
            &manager,
            "NODE02",
            DEVICE_TRANSPORT_WIFI,
            20,
            self
        ) == DEVICE_MANAGER_OK
    );


    /*
     * 旧fd=10现在才迟到执行disconnect。
     */
    assert(
        device_manager_disconnect(
            &manager,
            "NODE02",
            10,
            self
        ) == DEVICE_MANAGER_CONFLICT
    );


    /*
     * 新连接必须仍然在线。
     */
    assert(
        device_manager_find(
            &manager,
            "NODE02",
            &device
        ) == DEVICE_MANAGER_OK
    );

    assert(
        device.online == 1
    );

    assert(
        device.fd == 20
    );


    device_manager_destroy(
        &manager
    );

    printf(
        "[PASS] stale disconnect protection\n"
    );
}

static void test_command_queue_fifo(void)
{
    device_manager_t manager;

    frame_command_t command1;
    frame_command_t command2;
    frame_command_t output;

    pthread_t self =
        pthread_self();


    assert(
        device_manager_init(
            &manager
        ) == DEVICE_MANAGER_OK
    );


    assert(
        device_manager_bind_connection(
            &manager,
            "NODE01",
            DEVICE_TRANSPORT_BLUETOOTH,
            10,
            self
        ) == DEVICE_MANAGER_OK
    );


    memset(
        &command1,
        0,
        sizeof(command1)
    );

    strcpy(
        command1.target_node,
        "NODE01"
    );

    strcpy(
        command1.sender,
        "GATEWAY"
    );

    command1.sequence =
        100;


    memset(
        &command2,
        0,
        sizeof(command2)
    );

    strcpy(
        command2.target_node,
        "NODE01"
    );

    strcpy(
        command2.sender,
        "GATEWAY"
    );

    command2.sequence =
        101;


    /*
     * 入队顺序：100 -> 101
     */
    assert(
        device_manager_enqueue_command(
            &manager,
            "NODE01",
            &command1
        ) == DEVICE_MANAGER_OK
    );

    assert(
        device_manager_enqueue_command(
            &manager,
            "NODE01",
            &command2
        ) == DEVICE_MANAGER_OK
    );


    /*
     * 出队必须仍然是100 -> 101。
     */
    assert(
        device_manager_try_dequeue_command(
            &manager,
            "NODE01",
            10,
            self,
            &output
        ) == DEVICE_MANAGER_OK
    );

    assert(
        output.sequence == 100U
    );


    assert(
        device_manager_try_dequeue_command(
            &manager,
            "NODE01",
            10,
            self,
            &output
        ) == DEVICE_MANAGER_OK
    );

    assert(
        output.sequence == 101U
    );


    /*
     * 已经空了。
     */
    assert(
        device_manager_try_dequeue_command(
            &manager,
            "NODE01",
            10,
            self,
            &output
        ) == DEVICE_MANAGER_QUEUE_EMPTY
    );


    device_manager_destroy(
        &manager
    );

    printf(
        "[PASS] command queue FIFO\n"
    );
}

static void test_command_queue_disconnect_clear(void)
{
    device_manager_t manager;

    frame_command_t command;
    frame_command_t output;

    pthread_t self =
        pthread_self();


    assert(
        device_manager_init(
            &manager
        ) == DEVICE_MANAGER_OK
    );


    assert(
        device_manager_bind_connection(
            &manager,
            "NODE02",
            DEVICE_TRANSPORT_WIFI,
            20,
            self
        ) == DEVICE_MANAGER_OK
    );


    memset(
        &command,
        0,
        sizeof(command)
    );

    strcpy(
        command.target_node,
        "NODE02"
    );

    strcpy(
        command.sender,
        "GATEWAY"
    );

    command.sequence =
        200;


    assert(
        device_manager_enqueue_command(
            &manager,
            "NODE02",
            &command
        ) == DEVICE_MANAGER_OK
    );


    /*
     * 还没有取命令就断线。
     */
    assert(
        device_manager_disconnect(
            &manager,
            "NODE02",
            20,
            self
        ) == DEVICE_MANAGER_OK
    );


    /*
     * NODE02重新连接。
     */
    assert(
        device_manager_bind_connection(
            &manager,
            "NODE02",
            DEVICE_TRANSPORT_WIFI,
            21,
            self
        ) == DEVICE_MANAGER_OK
    );


    /*
     * 旧命令不应该跟到新连接。
     */
    assert(
        device_manager_try_dequeue_command(
            &manager,
            "NODE02",
            21,
            self,
            &output
        ) == DEVICE_MANAGER_QUEUE_EMPTY
    );


    device_manager_destroy(
        &manager
    );

    printf(
        "[PASS] disconnect clears command queue\n"
    );
}

static void test_disconnect_collect_pending_commands(void)
{
    device_manager_t manager;

    frame_command_t command1;
    frame_command_t command2;

    frame_command_t pending[
        DEVICE_COMMAND_QUEUE_CAPACITY
    ];

    size_t pending_count;

    pthread_t self;


    assert(
        device_manager_init(
            &manager
        ) == DEVICE_MANAGER_OK
    );


    self =
        pthread_self();


    assert(
        device_manager_bind_connection(
            &manager,
            "NODE01",
            DEVICE_TRANSPORT_BLUETOOTH,
            10,
            self
        ) == DEVICE_MANAGER_OK
    );


    memset(
        &command1,
        0,
        sizeof(command1)
    );

    strcpy(
        command1.target_node,
        "NODE01"
    );

    strcpy(
        command1.sender,
        "GATEWAY"
    );

    command1.sequence = 100;


    memset(
        &command2,
        0,
        sizeof(command2)
    );

    strcpy(
        command2.target_node,
        "NODE01"
    );

    strcpy(
        command2.sender,
        "GATEWAY"
    );

    command2.sequence = 101;


    assert(
        device_manager_enqueue_command(
            &manager,
            "NODE01",
            &command1
        ) == DEVICE_MANAGER_OK
    );


    assert(
        device_manager_enqueue_command(
            &manager,
            "NODE01",
            &command2
        ) == DEVICE_MANAGER_OK
    );


    pending_count = 0U;


    assert(
        device_manager_disconnect_collect_commands(
            &manager,
            "NODE01",
            10,
            self,
            pending,
            DEVICE_COMMAND_QUEUE_CAPACITY,
            &pending_count
        ) == DEVICE_MANAGER_OK
    );


    assert(
        pending_count == 2U
    );


    /*
     * 必须保持FIFO。
     */
    assert(
        pending[0].sequence == 100
    );

    assert(
        pending[1].sequence == 101
    );


    device_manager_destroy(
        &manager
    );


    printf(
        "[PASS] disconnect collects pending commands\n"
    );
}

static void test_stale_disconnect_does_not_drain_queue(void)
{
    device_manager_t manager;

    frame_command_t command;

    frame_command_t pending[
        DEVICE_COMMAND_QUEUE_CAPACITY
    ];

    size_t pending_count;

    pthread_t self;


    assert(
        device_manager_init(
            &manager
        ) == DEVICE_MANAGER_OK
    );


    self =
        pthread_self();


    /*
     * 当前有效连接是fd=20。
     */
    assert(
        device_manager_bind_connection(
            &manager,
            "NODE02",
            DEVICE_TRANSPORT_WIFI,
            20,
            self
        ) == DEVICE_MANAGER_OK
    );


    memset(
        &command,
        0,
        sizeof(command)
    );

    strcpy(
        command.target_node,
        "NODE02"
    );

    strcpy(
        command.sender,
        "GATEWAY"
    );

    command.sequence = 200;


    assert(
        device_manager_enqueue_command(
            &manager,
            "NODE02",
            &command
        ) == DEVICE_MANAGER_OK
    );


    /*
     * 模拟旧连接fd=19退出。
     *
     * 它不是当前owner，因此必须失败。
     */
    pending_count = 0U;


    assert(
        device_manager_disconnect_collect_commands(
            &manager,
            "NODE02",
            19,
            self,
            pending,
            DEVICE_COMMAND_QUEUE_CAPACITY,
            &pending_count
        ) == DEVICE_MANAGER_CONFLICT
    );


    assert(
        pending_count == 0U
    );


    /*
     * 当前真实owner仍然应该能取到seq=200。
     */
    assert(
        device_manager_try_dequeue_command(
            &manager,
            "NODE02",
            20,
            self,
            &command
        ) == DEVICE_MANAGER_OK
    );


    assert(
        command.sequence == 200
    );


    device_manager_destroy(
        &manager
    );


    printf(
        "[PASS] stale disconnect preserves new queue\n"
    );
}

/*
 * bind_connection_collect_commands：全新节点首次注册。
 *
 * 此时没有旧连接，因此不应收集到任何 pending 命令。
 */
static void test_bind_collect_new_node(void)
{
    device_manager_t manager;
    device_t device;

    frame_command_t pending[DEVICE_COMMAND_QUEUE_CAPACITY];
    size_t pending_count = 0U;

    pthread_t self = pthread_self();

    assert(
        device_manager_init(&manager) == DEVICE_MANAGER_OK
    );

    assert(
        device_manager_bind_connection_collect_commands(
            &manager,
            "NODE01",
            DEVICE_TRANSPORT_BLUETOOTH,
            10,
            self,
            pending,
            DEVICE_COMMAND_QUEUE_CAPACITY,
            &pending_count
        ) == DEVICE_MANAGER_OK
    );

    /* 首次注册：没有旧连接，自然收不到命令 */
    assert(pending_count == 0U);
    assert(manager.count == 1U);

    assert(
        device_manager_find(&manager, "NODE01", &device) ==
        DEVICE_MANAGER_OK
    );

    assert(device.online == 1);
    assert(device.fd == 10);

    device_manager_destroy(&manager);

    printf("[PASS] bind collect: new node\n");
}

/*
 * 场景 A（本函数最关键的守卫）：
 * 同一个连接、同一个 fd/thread 反复 bind，
 * 属于幂等操作，**绝不允许清空 command queue**。
 *
 * 这是最容易在后续重构中被误删的分支：
 * 如果把它错当成 takeover，活连接里待发的命令会被静默丢弃。
 */
static void test_bind_collect_same_connection_keeps_queue(void)
{
    device_manager_t manager;

    frame_command_t command;
    frame_command_t pending[DEVICE_COMMAND_QUEUE_CAPACITY];
    frame_command_t output;

    size_t pending_count = 0U;

    pthread_t self = pthread_self();

    assert(
        device_manager_init(&manager) == DEVICE_MANAGER_OK
    );

    assert(
        device_manager_bind_connection(
            &manager,
            "NODE02",
            DEVICE_TRANSPORT_WIFI,
            20,
            self
        ) == DEVICE_MANAGER_OK
    );

    memset(&command, 0, sizeof(command));
    strcpy(command.target_node, "NODE02");
    strcpy(command.sender, "GATEWAY");
    command.sequence = 300;

    assert(
        device_manager_enqueue_command(
            &manager,
            "NODE02",
            &command
        ) == DEVICE_MANAGER_OK
    );

    /*
     * 同一个 fd + 同一个线程再次 bind。
     */
    pending_count = 0U;

    assert(
        device_manager_bind_connection_collect_commands(
            &manager,
            "NODE02",
            DEVICE_TRANSPORT_WIFI,
            20,          /* 同一个 fd */
            self,        /* 同一个线程 */
            pending,
            DEVICE_COMMAND_QUEUE_CAPACITY,
            &pending_count
        ) == DEVICE_MANAGER_OK
    );

    /* 幂等 bind：不应该收集到任何"被取代"的命令 */
    assert(pending_count == 0U);

    /* 关键：队列里的命令必须还在，仍然取得到 */
    assert(
        device_manager_try_dequeue_command(
            &manager,
            "NODE02",
            20,
            self,
            &output
        ) == DEVICE_MANAGER_OK
    );

    assert(output.sequence == 300);

    device_manager_destroy(&manager);

    printf("[PASS] bind collect: same connection keeps queue\n");
}

/*
 * 场景 C：新连接接管在线旧连接（takeover）。
 *
 * 旧连接队列里未发送的命令必须被**交出来**（而不是静默丢弃），
 * 由调用方回 NACK —— 这正是 C5-4 要解决的缺口。
 */
static void test_bind_collect_takeover_returns_stale(void)
{
    device_manager_t manager;
    device_t device;

    frame_command_t command1;
    frame_command_t command2;

    frame_command_t pending[DEVICE_COMMAND_QUEUE_CAPACITY];

    size_t pending_count = 0U;

    pthread_t self = pthread_self();

    assert(
        device_manager_init(&manager) == DEVICE_MANAGER_OK
    );

    /* 旧连接 fd=30 */
    assert(
        device_manager_bind_connection(
            &manager,
            "NODE02",
            DEVICE_TRANSPORT_WIFI,
            30,
            self
        ) == DEVICE_MANAGER_OK
    );

    memset(&command1, 0, sizeof(command1));
    strcpy(command1.target_node, "NODE02");
    strcpy(command1.sender, "GATEWAY");
    command1.sequence = 400;

    memset(&command2, 0, sizeof(command2));
    strcpy(command2.target_node, "NODE02");
    strcpy(command2.sender, "GATEWAY");
    command2.sequence = 401;

    assert(
        device_manager_enqueue_command(
            &manager, "NODE02", &command1
        ) == DEVICE_MANAGER_OK
    );

    assert(
        device_manager_enqueue_command(
            &manager, "NODE02", &command2
        ) == DEVICE_MANAGER_OK
    );

    /*
     * 新连接 fd=31 接管。旧连接此刻仍是在线的。
     */
    assert(
        device_manager_bind_connection_collect_commands(
            &manager,
            "NODE02",
            DEVICE_TRANSPORT_WIFI,
            31,
            self,
            pending,
            DEVICE_COMMAND_QUEUE_CAPACITY,
            &pending_count
        ) == DEVICE_MANAGER_OK
    );

    /* 旧连接积压的 2 条必须被交出来，且保持 FIFO */
    assert(pending_count == 2U);
    assert(pending[0].sequence == 400);
    assert(pending[1].sequence == 401);

    /* 新连接成为 owner，队列已清空 */
    assert(
        device_manager_find(&manager, "NODE02", &device) ==
        DEVICE_MANAGER_OK
    );

    assert(device.online == 1);
    assert(device.fd == 31);

    {
        frame_command_t output;

        assert(
            device_manager_try_dequeue_command(
                &manager,
                "NODE02",
                31,
                self,
                &output
            ) == DEVICE_MANAGER_QUEUE_EMPTY
        );
    }

    device_manager_destroy(&manager);

    printf("[PASS] bind collect: takeover returns stale commands\n");
}

/*
 * 场景 B：设备离线后重连。
 *
 * fd 会变化，旧 queue 在 disconnect 时已被清空，
 * 因此重连时不应收集到命令。
 */
static void test_bind_collect_after_offline(void)
{
    device_manager_t manager;
    device_t device;

    frame_command_t command;
    frame_command_t pending[DEVICE_COMMAND_QUEUE_CAPACITY];

    size_t pending_count = 0U;

    pthread_t self = pthread_self();

    assert(
        device_manager_init(&manager) == DEVICE_MANAGER_OK
    );

    assert(
        device_manager_bind_connection(
            &manager,
            "NODE02",
            DEVICE_TRANSPORT_WIFI,
            40,
            self
        ) == DEVICE_MANAGER_OK
    );

    memset(&command, 0, sizeof(command));
    strcpy(command.target_node, "NODE02");
    strcpy(command.sender, "GATEWAY");
    command.sequence = 500;

    assert(
        device_manager_enqueue_command(
            &manager, "NODE02", &command
        ) == DEVICE_MANAGER_OK
    );

    /* 断线：此时命令已被 disconnect_collect 取走并清空 */
    {
        frame_command_t drained[DEVICE_COMMAND_QUEUE_CAPACITY];
        size_t drained_count = 0U;

        assert(
            device_manager_disconnect_collect_commands(
                &manager,
                "NODE02",
                40,
                self,
                drained,
                DEVICE_COMMAND_QUEUE_CAPACITY,
                &drained_count
            ) == DEVICE_MANAGER_OK
        );

        assert(drained_count == 1U);
        assert(drained[0].sequence == 500);
    }

    /* 离线后重连，新 fd=41 */
    pending_count = 0U;

    assert(
        device_manager_bind_connection_collect_commands(
            &manager,
            "NODE02",
            DEVICE_TRANSPORT_WIFI,
            41,
            self,
            pending,
            DEVICE_COMMAND_QUEUE_CAPACITY,
            &pending_count
        ) == DEVICE_MANAGER_OK
    );

    /* 队列已在断线时清空，重连不应再收集到东西 */
    assert(pending_count == 0U);

    assert(
        device_manager_find(&manager, "NODE02", &device) ==
        DEVICE_MANAGER_OK
    );

    assert(device.online == 1);
    assert(device.fd == 41);

    device_manager_destroy(&manager);

    printf("[PASS] bind collect: after offline\n");
}

/*
 * 容量不足时必须**什么都不改**：
 * 不清队列、不换 owner。
 *
 * 否则旧命令会在没有交给调用方的情况下丢失 —— 正是要避免的静默丢弃。
 */
static void test_bind_collect_buffer_too_small(void)
{
    device_manager_t manager;
    device_t device;

    frame_command_t command1;
    frame_command_t command2;
    frame_command_t small[1];

    size_t pending_count = 99U;

    pthread_t self = pthread_self();

    assert(
        device_manager_init(&manager) == DEVICE_MANAGER_OK
    );

    assert(
        device_manager_bind_connection(
            &manager,
            "NODE02",
            DEVICE_TRANSPORT_WIFI,
            50,
            self
        ) == DEVICE_MANAGER_OK
    );

    memset(&command1, 0, sizeof(command1));
    strcpy(command1.target_node, "NODE02");
    strcpy(command1.sender, "GATEWAY");
    command1.sequence = 600;

    memset(&command2, 0, sizeof(command2));
    strcpy(command2.target_node, "NODE02");
    strcpy(command2.sender, "GATEWAY");
    command2.sequence = 601;

    assert(
        device_manager_enqueue_command(
            &manager, "NODE02", &command1
        ) == DEVICE_MANAGER_OK
    );

    assert(
        device_manager_enqueue_command(
            &manager, "NODE02", &command2
        ) == DEVICE_MANAGER_OK
    );

    /* 只给 1 个位置，但积压了 2 条 */
    assert(
        device_manager_bind_connection_collect_commands(
            &manager,
            "NODE02",
            DEVICE_TRANSPORT_WIFI,
            51,
            self,
            small,
            1U,
            &pending_count
        ) == DEVICE_MANAGER_BUFFER_TOO_SMALL
    );

    /* 失败时 out_count 必须是 0，不能留下脏值 */
    assert(pending_count == 0U);

    /*
     * 关键：状态必须完全没有变化 ——
     * 旧连接仍在线、仍是 owner、命令还在队列里。
     */
    assert(
        device_manager_find(&manager, "NODE02", &device) ==
        DEVICE_MANAGER_OK
    );

    assert(device.online == 1);
    assert(device.fd == 50);

    {
        frame_command_t output;

        assert(
            device_manager_try_dequeue_command(
                &manager,
                "NODE02",
                50,
                self,
                &output
            ) == DEVICE_MANAGER_OK
        );

        assert(output.sequence == 600);
    }

    device_manager_destroy(&manager);

    printf("[PASS] bind collect: buffer too small keeps state\n");
}

int main(void)
{
    printf(
        "Running device_manager tests...\n"
    );

    test_register_and_find();
    test_duplicate_register();
    test_online_offline();
    test_unregister();
    test_last_seen();
    test_capacity_full();
    test_concurrent_access();
    test_reconnect();
    test_stale_disconnect();
    test_command_queue_fifo();
    test_command_queue_disconnect_clear();
    test_disconnect_collect_pending_commands();
    test_stale_disconnect_does_not_drain_queue();
    test_bind_collect_new_node();
    test_bind_collect_same_connection_keeps_queue();
    test_bind_collect_takeover_returns_stale();
    test_bind_collect_after_offline();
    test_bind_collect_buffer_too_small();

    printf(
        "All device_manager tests passed.\n"
    );

    return 0;
}