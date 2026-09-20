#define _POSIX_C_SOURCE 200809L

#include "message_queue.h"

#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

typedef struct 
{
    message_queue_t *queue;
    int result;
}consumer_thread_arg_t;

/*
 *构建一条测试消息
 */

 static gateway_message_t make_message(unsigned int value)
 {
    gateway_message_t message;
    int written;

    memset(&message,0,sizeof(message));

    written = snprintf(
        (char *)message.data,
        sizeof(message.data),
        "MSG-%u",
        value
    );

    assert(written > 0);
    assert(
        (size_t)written < sizeof(message.data)
    );

    message.length = (size_t)written;

    return message;
 }

 /*
  *测试最基本的FIFO
  *
  * push 1
  * push 2
  * push 3
  * 
  * pop必须得到
  * 1->2->3
  */
 static void test_basic_fifo(void)
 {
    message_queue_t queue;

    gateway_message_t message;
    gateway_message_t output;

    assert(message_queue_init(&queue)==MESSAGE_QUEUE_OK);

    message = make_message(1);

    assert(message_queue_push(&queue,&message) == MESSAGE_QUEUE_OK);

    message = make_message(2);

    assert(message_queue_push(&queue,&message)==MESSAGE_QUEUE_OK);

    message = make_message(3);

    assert(message_queue_push(&queue,&message)==MESSAGE_QUEUE_OK);

    assert(message_queue_pop(&queue,&output)==MESSAGE_QUEUE_OK);

    assert(strcmp((const char *)output.data,"MSG-1")==0);

    assert(message_queue_pop(&queue,&output)==MESSAGE_QUEUE_OK);

    assert(strcmp((const char *)output.data,"MSG-2")==0);

    assert(message_queue_pop(&queue,&output)==MESSAGE_QUEUE_OK);

    assert(strcmp((const char *)output.data,"MSG-3")==0);

    message_queue_destroy(&queue);

    printf("PASS basic FIFO\n");

 }

 /*
 * 测试环形回绕。
 *
 * 先填满 0~31。
 *
 * 再取走 0~15。
 *
 * 然后加入 32~47。
 *
 * 此时 tail 必须从数组尾部重新绕回开头。
 */

static void test_wraparound(void)
{
    message_queue_t queue;

    gateway_message_t message;
    gateway_message_t output;

    unsigned int i;

    assert(message_queue_init(&queue)==MESSAGE_QUEUE_OK);

    //填满整个队列

    for(i=0; i<MESSAGE_QUEUE_CAPACITY;i++)
    {
        message= make_message(i);
        assert(message_queue_push(&queue,&message)==MESSAGE_QUEUE_OK);
    }

    //取一半
    for(i=0; i<MESSAGE_QUEUE_CAPACITY/2U;i++)
    {
        char expected[GATEWAY_MESSAGE_MAX_SIZE];
        assert(message_queue_pop(&queue,&output)==MESSAGE_QUEUE_OK);

        snprintf(expected,sizeof(expected),"MSG-%u",i);

        assert(strcmp((const char *)output.data,expected)==0);

    }

 /*
     * 再加入半个队列，
     * tail 此时应该发生回绕。
     */
    for (i = MESSAGE_QUEUE_CAPACITY;
         i <
         MESSAGE_QUEUE_CAPACITY +
         MESSAGE_QUEUE_CAPACITY / 2U;
         i++)
    {
        message = make_message(i);

        assert(
            message_queue_push(
                &queue,
                &message
            ) == MESSAGE_QUEUE_OK
        );
    }


    /*
     * 剩余消息必须继续保持 FIFO。
     */
    for (i = MESSAGE_QUEUE_CAPACITY / 2U;
         i <
         MESSAGE_QUEUE_CAPACITY +
         MESSAGE_QUEUE_CAPACITY / 2U;
         i++)
    {
        char expected[
            GATEWAY_MESSAGE_MAX_SIZE
        ];

        assert(
            message_queue_pop(
                &queue,
                &output
            ) == MESSAGE_QUEUE_OK
        );

        snprintf(
            expected,
            sizeof(expected),
            "MSG-%u",
            i
        );

        assert(
            strcmp(
                (const char *)output.data,
                expected
            ) == 0
        );
    }

    message_queue_destroy(
        &queue
    );

    printf(
        "[PASS] ring wraparound\n"
    );
}

//pop一个
static void *blocking_consumer(
    void *arg
)
{
    consumer_thread_arg_t *thread_arg;
    gateway_message_t message;

    thread_arg = (consumer_thread_arg_t *)arg;

    thread_arg->result = message_queue_pop(thread_arg->queue,&message);

    return NULL;
}

static void test_shutdown_wakes_consumer(void)
{
    message_queue_t queue;

    pthread_t thread;

    consumer_thread_arg_t arg;

    struct timespec delay;

    assert(
        message_queue_init(&queue) ==
        MESSAGE_QUEUE_OK
    );

    arg.queue = &queue;
    arg.result = MESSAGE_QUEUE_ERROR;

    /*
     * pthread_create 成功返回 0，
     * 因此必须断言 == 0，否则断言方向是反的。
     */
    assert(
        pthread_create(
            &thread,
            NULL,
            blocking_consumer,
            &arg
        ) == 0
    );

    /*
     * 给 consumer 一点时间进入：
     *
     * pthread_cond_wait(not_empty)
     */

    delay.tv_sec = 0;
    delay.tv_nsec = 100000000L;

    nanosleep(&delay,NULL);

    /*
     * 此时队列还是空的。
     *
     * consumer 应该阻塞在 pop()。
     */
    message_queue_shutdown(
        &queue
    );

    /*
     * shutdown 应该 broadcast，
     * 因而这里能够正常 join。
     */

    assert(
        pthread_join(
            thread,
            NULL
        ) == 0
    );

    assert(
        arg.result ==
        MESSAGE_QUEUE_SHUTDOWN
    );

    message_queue_destroy(
        &queue
    );

    printf(
        "[PASS] shutdown wakes consumer\n"
    );
}

/*
 *测试非阻塞取消息 try_pop 的三种返回路径。
 *
 *这是 server_link_worker 的核心依赖：
 *队列空时必须立即返回 EMPTY（不能阻塞），
 *否则 worker 无法继续处理服务器下行数据。
 */
static void test_try_pop(void)
{
    message_queue_t queue;

    gateway_message_t message;
    gateway_message_t output;

    assert(
        message_queue_init(&queue) ==
        MESSAGE_QUEUE_OK
    );

    /*
     * 1) 空队列：必须立即返回 MESSAGE_QUEUE_EMPTY，而不是阻塞。
     */
    assert(
        message_queue_try_pop(&queue, &output) ==
        MESSAGE_QUEUE_EMPTY
    );

    /*
     * 2) 有消息：取出并保持 FIFO 顺序。
     */
    message = make_message(1);
    assert(
        message_queue_push(&queue, &message) ==
        MESSAGE_QUEUE_OK
    );

    message = make_message(2);
    assert(
        message_queue_push(&queue, &message) ==
        MESSAGE_QUEUE_OK
    );

    assert(
        message_queue_try_pop(&queue, &output) ==
        MESSAGE_QUEUE_OK
    );
    assert(strcmp((const char *)output.data, "MSG-1") == 0);

    /*
     * 3) 取空之后再次调用，仍应返回 EMPTY（回到空状态）。
     */
    assert(
        message_queue_try_pop(&queue, &output) ==
        MESSAGE_QUEUE_OK
    );
    assert(strcmp((const char *)output.data, "MSG-2") == 0);

    assert(
        message_queue_try_pop(&queue, &output) ==
        MESSAGE_QUEUE_EMPTY
    );

    /*
     * 4) shutdown 且空：返回 SHUTDOWN，让消费者知道可以退出。
     *    这里同时验证 shutdown 之前不会误报。
     */
    message_queue_shutdown(&queue);

    assert(
        message_queue_try_pop(&queue, &output) ==
        MESSAGE_QUEUE_SHUTDOWN
    );

    /*
     * 5) shutdown 但仍有存量消息：
     *    必须允许把存量取完，不能直接返回 SHUTDOWN。
     */
    {
        message_queue_t queue2;

        assert(
            message_queue_init(&queue2) ==
            MESSAGE_QUEUE_OK
        );

        message = make_message(7);
        assert(
            message_queue_push(&queue2, &message) ==
            MESSAGE_QUEUE_OK
        );

        message_queue_shutdown(&queue2);

        assert(
            message_queue_try_pop(&queue2, &output) ==
            MESSAGE_QUEUE_OK
        );
        assert(strcmp((const char *)output.data, "MSG-7") == 0);

        assert(
            message_queue_try_pop(&queue2, &output) ==
            MESSAGE_QUEUE_SHUTDOWN
        );

        message_queue_destroy(&queue2);
    }

    /*
     * 6) 参数校验。
     */
    assert(
        message_queue_try_pop(NULL, &output) ==
        MESSAGE_QUEUE_ERROR
    );
    assert(
        message_queue_try_pop(&queue, NULL) ==
        MESSAGE_QUEUE_ERROR
    );

    message_queue_destroy(&queue);

    printf(
        "[PASS] try_pop non-blocking paths\n"
    );
}

/*
 *验证 try_pop 在队列为空时确实不阻塞。
 *
 *做法：先记录时间，调用一次 try_pop，
 *若实现误用 pthread_cond_wait，这里会永久挂住，
 *外层的 timeout 会判定为死锁。
 */
static void test_try_pop_does_not_block(void)
{
    message_queue_t queue;
    gateway_message_t output;
    struct timespec start;
    struct timespec end;
    long elapsed_ms;

    assert(
        message_queue_init(&queue) ==
        MESSAGE_QUEUE_OK
    );

    assert(
        clock_gettime(CLOCK_MONOTONIC, &start) == 0
    );

    assert(
        message_queue_try_pop(&queue, &output) ==
        MESSAGE_QUEUE_EMPTY
    );

    assert(
        clock_gettime(CLOCK_MONOTONIC, &end) == 0
    );

    elapsed_ms =
        (end.tv_sec - start.tv_sec) * 1000L +
        (end.tv_nsec - start.tv_nsec) / 1000000L;

    /*
     * 空队列取消息应当在极短时间内返回。
     * 这里放宽到 100ms，只为捕获"阻塞睡眠"这类错误实现。
     */
    assert(elapsed_ms < 100);

    message_queue_destroy(&queue);

    printf(
        "[PASS] try_pop returns immediately when empty\n"
    );
}

int main(void)
{
    printf(
        "Running message_queue tests...\n"
    );

    test_basic_fifo();

    test_wraparound();

    test_shutdown_wakes_consumer();

    test_try_pop();

    test_try_pop_does_not_block();

    printf(
        "All message_queue tests passed.\n"
    );

    return 0;
}