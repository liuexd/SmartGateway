#include "message_queue.h"

#include <stddef.h>

int message_queue_init(message_queue_t *queue)
{
    int result;

    if(queue == NULL)
    {
        return MESSAGE_QUEUE_ERROR;
    }

    queue->head = 0;
    queue->tail = 0;
    queue->count = 0;
    queue->shutting_down =0;
    
    result = pthread_mutex_init(
        &queue->mutex,
        NULL
    );

    /*
     *mutex初始化失败时不能调用pthread_mutex_destroy，
     *因为此时mutex并未成功初始化，属于未定义行为。
     */
    if(result != 0)
    {
        return MESSAGE_QUEUE_ERROR;
    }

    result = pthread_cond_init(&queue->not_empty,NULL);
    if(result != 0)
    {
       pthread_mutex_destroy(&queue->mutex);
       return MESSAGE_QUEUE_ERROR;
    }

    result = pthread_cond_init(&queue->not_full,NULL);
    if(result != 0)
    {
        pthread_cond_destroy(&queue->not_empty);

        pthread_mutex_destroy(&queue->mutex);

        return MESSAGE_QUEUE_ERROR;
    }

    return MESSAGE_QUEUE_OK;
}

int message_queue_push(message_queue_t *queue,
    const gateway_message_t *message)
{
    if(queue == NULL || message == NULL || message->length > GATEWAY_MESSAGE_MAX_SIZE)
    {
        return MESSAGE_QUEUE_ERROR;
    }

    pthread_mutex_lock(&queue->mutex);

    /*
     * 队列满时不能直接覆盖旧消息。
     *
     * 必须等待消费者 pop，
     * 直到出现空位或者 queue shutdown。
     */
    while(queue->count == MESSAGE_QUEUE_CAPACITY && !queue->shutting_down)
    {
        pthread_cond_wait(
            &queue->not_full,
            &queue->mutex
        );
    }

    /*
     *shutdown后不再接收新的消息    
     */
    if(queue->shutting_down)
    {
        pthread_mutex_unlock(&queue->mutex);

        return MESSAGE_QUEUE_SHUTDOWN;
    }

    /*
     *写入tail
     */

     queue->items[queue->tail] = *message;

     /*
      *环形移动
      */
     queue->tail = (queue->tail + 1U)% MESSAGE_QUEUE_CAPACITY;

     queue->count++;

     /*
      *现在队列已经非空
      *唤醒一个等待pop的消费者
      */

      pthread_cond_signal(&queue->not_empty);

      pthread_mutex_unlock(&queue->mutex);

      return MESSAGE_QUEUE_OK;
}

int message_queue_pop(message_queue_t *queue, gateway_message_t *message)
{
    if(queue == NULL || message == NULL)
    {
        return MESSAGE_QUEUE_ERROR;
    }

    pthread_mutex_lock(&queue->mutex);

    /*
     *队列为空时，
     *消费者等待生产着push
     */

    while(queue->count == 0  && !queue->shutting_down)
    {
        pthread_cond_wait(&queue->not_empty,
            &queue->mutex
        );
    }

    /*
     *shutdown且没有剩余消息
     *消费者可以退出
     *
     *这里必须返回MESSAGE_QUEUE_SHUTDOWN而不是MESSAGE_QUEUE_ERROR，
     *否则调用方无法区分"正常收工"和"参数非法"。
     */
    if(queue->count == 0 && queue->shutting_down)
    {
        pthread_mutex_unlock(&queue->mutex);

        return MESSAGE_QUEUE_SHUTDOWN;
    }

    *message = queue->items[queue->head];

    queue->head = (queue->head + 1U)%MESSAGE_QUEUE_CAPACITY;

    queue->count--;

    /*
     *队列现在有空位，唤醒一个等待push的生产者
     */
    pthread_cond_signal(&queue->not_full);

    pthread_mutex_unlock(&queue->mutex);

    return MESSAGE_QUEUE_OK;
}

int message_queue_try_pop(
    message_queue_t *queue,
    gateway_message_t *message
)
{
    int result;

    if(queue == NULL || message == NULL)
    {
        return MESSAGE_QUEUE_ERROR;
    }

    pthread_mutex_lock(&queue->mutex);

    if(queue->count == 0)
    {
        if(queue->shutting_down)
        {
            result = MESSAGE_QUEUE_SHUTDOWN;
        }
        else
        {
            result = MESSAGE_QUEUE_EMPTY;
        }

        /*
         *所有分支都必须解锁后再返回，
         *否则调用方下次进入会死锁。
         */
        pthread_mutex_unlock(&queue->mutex);

        return result;
    }

    *message  = queue->items[queue->head];
    queue->head = (queue->head + 1U)%MESSAGE_QUEUE_CAPACITY;
    queue->count --;

    /*
     * pop 出一条以后产生空位，
     * 唤醒可能阻塞的 producer。
     */
    pthread_cond_signal(&queue->not_full);

    pthread_mutex_unlock(&queue->mutex);

    return MESSAGE_QUEUE_OK;
}

void message_queue_shutdown(message_queue_t *queue)
{
    if(queue == NULL)
    {
        return;
    }

    pthread_mutex_lock(&queue->mutex);

    queue->shutting_down = 1;

    /*
     *两边都进行broadcast
     *因为此时可能
     *productcer卡在not_full
     *consumer卡在not——empty
     */

    pthread_cond_broadcast(
        &queue->not_empty
    );
    pthread_cond_broadcast(
        &queue->not_full 
    );

    pthread_mutex_unlock(&queue->mutex);
}

void message_queue_destroy(
    message_queue_t *queue
)
{
    if(queue == NULL)
    {
        return;
    }

    pthread_cond_destroy(&queue->not_empty);

    pthread_cond_destroy(&queue->not_full);

    pthread_mutex_destroy(&queue->mutex);

}