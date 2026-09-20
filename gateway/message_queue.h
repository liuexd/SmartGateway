#ifndef MESSAGE_QUEUE_H
#define MESSAGE_QUEUE_H


#include <stddef.h>
#include <pthread.h>
/*
 * message_queue.h
 *第一版本采用固定容量的有界环形队列，后续改成动态扩容的无界队列。
 * 
 *     
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

    size_t head;
    size_t tail;
    size_t count;

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
 * 向队列中推送消息
 * @param queue 队列指针
 * @param message 消息指针
 * @return 返回值
 */
int message_queue_push(message_queue_t *queue, const gateway_message_t *message);

/**
 * 从队列中弹出消息
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
 * 关闭队列
 * @param queue 队列指针
 */
void message_queue_shutdown(message_queue_t *queue);

/**
 * 销毁队列
 * @param queue 队列指针
 */
void message_queue_destroy(message_queue_t *queue);

#endif 