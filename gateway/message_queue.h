/* 模块 message_queue：固定容量线程安全上行队列；生产者与 Server Link 消费者共享。 */
#ifndef MESSAGE_QUEUE_H
#define MESSAGE_QUEUE_H


#include <stddef.h>
#include <pthread.h>
/*
 * message_queue.h
 * 固定容量的有界环形队列，汇集设备 worker 的上行消息。
 */

#define MESSAGE_QUEUE_CAPACITY 32

#define GATEWAY_MESSAGE_MAX_SIZE 256

/*
 *队列接口返回值
 */

#define MESSAGE_QUEUE_OK 0
#define MESSAGE_QUEUE_SHUTDOWN 1
#define MESSAGE_QUEUE_EMPTY 2
#define MESSAGE_QUEUE_ERROR -1


/*
 *Gateway内部上行信息
 */
typedef struct
{
    unsigned char data[GATEWAY_MESSAGE_MAX_SIZE];
    size_t length;

} gateway_message_t;

/*
 *有界环形消息队列
 */

typedef struct
{
    gateway_message_t items[MESSAGE_QUEUE_CAPACITY];

    size_t head;  /* 下一次读取的位置 */
    size_t tail;  /* 下一次写入的位置 */
    size_t count; /* 当前消息数；以上字段和 shutting_down 均受 mutex 保护 */

    pthread_mutex_t mutex;

    /*
     *队列非空：
     *唤醒等待的pop的消费者
     */

    pthread_cond_t not_empty;

     /*
     *队列非满：
     *唤醒等待的push的生产者
     */
    pthread_cond_t not_full;

    int shutting_down;
    

} message_queue_t;
/*
 *初始化队列
 */
int message_queue_init(message_queue_t *queue);
/**
 * 向队列中复制消息；满时等待空位或 shutdown。
 * @param queue 队列指针
 * @param message 消息指针
 * @return 返回值
 */
int message_queue_push(message_queue_t *queue, const gateway_message_t *message);

/**
 * 从队列中弹出消息；空时等待消息或 shutdown。
 * @param queue 队列指针
 * @param message 消息指针
 * @return 返回值
 */
int message_queue_pop(message_queue_t *queue, gateway_message_t *message);

/*
 * 非阻塞取消息。
 *
 * 有消息：
 *     MESSAGE_QUEUE_OK
 *
 * 队列为空：
 *     MESSAGE_QUEUE_EMPTY
 *
 * shutdown 且已经没有消息：
 *     MESSAGE_QUEUE_SHUTDOWN
 */
int message_queue_try_pop(
    message_queue_t *queue,
    gateway_message_t *message
);

/**
 * 禁止新消息并广播两个条件变量，唤醒等待空位或消息的线程。
 * 已入队消息仍可取出；此操作不销毁队列。
 * @param queue 队列指针
 */
void message_queue_shutdown(message_queue_t *queue);

/**
 * 销毁同步对象；调用前必须等待所有生产者和消费者退出。
 * @param queue 队列指针
 */
void message_queue_destroy(message_queue_t *queue);

#endif 