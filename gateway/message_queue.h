/* 模块 message_queue：固定容量的线程安全上行队列。 */
#ifndef MESSAGE_QUEUE_H
#define MESSAGE_QUEUE_H

#include <stddef.h>
#include <pthread.h>

#define MESSAGE_QUEUE_CAPACITY 32
#define GATEWAY_MESSAGE_MAX_SIZE 256

#define MESSAGE_QUEUE_OK 0
#define MESSAGE_QUEUE_SHUTDOWN 1
#define MESSAGE_QUEUE_EMPTY 2
#define MESSAGE_QUEUE_ERROR -1

/* 上行消息按值复制；length 是 data 中有效字节数。 */
typedef struct
{
    unsigned char data[GATEWAY_MESSAGE_MAX_SIZE];
    size_t length;
} gateway_message_t;

/* mutex 保护 items、head、tail、count 和 shutting_down。 */
typedef struct
{
    gateway_message_t items[MESSAGE_QUEUE_CAPACITY];
    size_t head;  /* 下一个读取位置 */
    size_t tail;  /* 下一个写入位置 */
    size_t count; /* 当前消息数 */
    pthread_mutex_t mutex;
    pthread_cond_t not_empty; /* 消费者等待消息 */
    pthread_cond_t not_full;  /* 生产者等待空位 */
    int shutting_down;        /* 非零时不再接受新消息 */
} message_queue_t;

/* 初始化 queue；成功返回 OK，参数无效或同步对象初始化失败返回 ERROR。 */
int message_queue_init(message_queue_t *queue);

/* 复制 message 到 queue；满时等待空位或 shutdown。
 * 返回 OK、SHUTDOWN 或 ERROR（参数无效或消息过大）。 */
int message_queue_push(message_queue_t *queue, const gateway_message_t *message);

/* 将队首消息复制到 message；空时等待消息或 shutdown。
 * 返回 OK、SHUTDOWN（关闭且已空）或 ERROR（参数无效）。 */
int message_queue_pop(message_queue_t *queue, gateway_message_t *message);

/* 不等待消息：返回 OK、EMPTY、SHUTDOWN（关闭且已空）或 ERROR。
 * 与 pop 一样，此操作仍须获取队列 mutex。 */
int message_queue_try_pop(message_queue_t *queue, gateway_message_t *message);

/* 禁止新消息并唤醒生产者和消费者；已入队消息仍可取出。
 * 此操作不销毁同步对象。queue 可为 NULL。 */
void message_queue_shutdown(message_queue_t *queue);

/* 所有生产者和消费者退出后销毁同步对象；queue 可为 NULL。 */
void message_queue_destroy(message_queue_t *queue);

#endif
