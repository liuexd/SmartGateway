#include "device_manager.h"

#include "string.h"

//这里的_lock代表着使用这个函数之前必须得上锁
static int device_manager_find_index_locked(
    const device_manager_t *manager,
    const char *node_id
)
{
    size_t i;

    for(i = 0; i < manager->count; i++)
    {
        if(strcmp(manager->devices[i].node_id,node_id) == 0)
        {
            return (int) i;
        }
    }

    return -1;
}

int device_manager_init(device_manager_t *manager)
{
    if(manager == NULL)
    {
        return DEVICE_MANAGER_ERROR;
    }

    memset(manager,0,sizeof(*manager));

    if(pthread_mutex_init(&manager->mutex,NULL)!=0)
    {
        return DEVICE_MANAGER_ERROR;
    }

    return DEVICE_MANAGER_OK;
}

void device_manager_destroy(device_manager_t *manager)
{
    if(manager == NULL)
    {
        return;
    }

    pthread_mutex_destroy(&manager->mutex);
}

int device_manager_find(
    device_manager_t *manager,
    const char *node_id,
    device_t *out_device
)
{
    int index;

    if (manager == NULL ||
        node_id == NULL ||
        node_id[0] == '\0' ||
        out_device == NULL)
    {
        return DEVICE_MANAGER_ERROR;
    }

    pthread_mutex_lock(&manager->mutex);

    index = device_manager_find_index_locked(manager,node_id);

    if(index < 0)
    {
        pthread_mutex_unlock(&manager->mutex);
        return DEVICE_MANAGER_NOT_FOUND;
    }

    *out_device = manager->devices[index];

    pthread_mutex_unlock(&manager->mutex);

    return DEVICE_MANAGER_OK;
}

int device_manager_register(
    device_manager_t *manager,
    const char *node_id,
    device_transport_t transport,
    int fd,
    pthread_t thread_id
)
{
    size_t node_id_length;
    int index;
    device_t *device;
    time_t now;

    if(manager == NULL ||
    node_id == NULL ||
    node_id[0] == '\0')
    {
        return DEVICE_MANAGER_ERROR;
    }

    node_id_length = strlen(node_id);

    //不允许静默截断node_id
    if(node_id_length >= DEVICE_NODE_ID_SIZE)
    {
        return DEVICE_MANAGER_ERROR;
    }

    now = time(NULL);

    if(now == (time_t) -1)
    {
        return DEVICE_MANAGER_ERROR;
    }

    pthread_mutex_lock(&manager->mutex);

    //同一个node_id不能重复注册
    index = device_manager_find_index_locked(manager,node_id);

    if(index >= 0)
    {
        pthread_mutex_unlock(&manager->mutex);
        return DEVICE_MANAGER_EXISTS;
    }

    if(manager->count == DEVICE_MANAGER_MAX_DEVICES)
    {
        pthread_mutex_unlock(&manager->mutex);
        return DEVICE_MANAGER_FULL;
    }

    device = &manager->devices[manager->count];

    memset(device,0,sizeof(*device));
    //
    memcpy(device->node_id,node_id,node_id_length + 1U);

    device->transport = transport;

    device->fd = fd;

    /*
     *fd有效时认为当前在线。
     *这样也允许以后用fd=-1
     *预注册一个离线设备
     */
    device->online = (fd>0) ? 1 : 0;

    device->thread_id = thread_id;

    device->last_seen = now;

    manager->count++;

    pthread_mutex_unlock(&manager->mutex);

    return DEVICE_MANAGER_OK;
}

int device_manager_unregister(
    device_manager_t *manager,
    const char *node_id
)
{
    int index;
    size_t i;

    if(manager == NULL ||
        node_id == NULL ||
        node_id[0] == '\0')
    {
        return DEVICE_MANAGER_ERROR;
    }

    pthread_mutex_lock(&manager->mutex);

    index = device_manager_find_index_locked(manager,node_id);
    if(index < 0)
    {
        pthread_mutex_unlock(&manager->mutex);
        return DEVICE_MANAGER_NOT_FOUND;
    }

    /*
     *删除数组中间元素以后
     *把后面的设备向前移动一格
     */
    for(i=(size_t)index; i+1U<manager->count;i++)
    {
        manager->devices[i] = manager->devices[i+1U];
    }
    manager->count--;

    /*
     *清理最后一个已经无效位置
     */
    memset(&manager->devices[manager->count],0,sizeof(manager->devices[0]));

    pthread_mutex_unlock(&manager->mutex);

    return DEVICE_MANAGER_OK;
}

int device_manager_set_online(
    device_manager_t *manager,
    const char *node_id,
    int online,
    int fd
)
{
    int index;

    if(manager == NULL||
        node_id == NULL ||
        node_id[0] == '\0' ||
        (online != 0 && online != 1))
    {
        return DEVICE_MANAGER_ERROR;
    }

    /*
     *设备声明为在线时
     *必须存在有效fd
     */
    if(online && fd < 0)
    {
        return DEVICE_MANAGER_ERROR;
    }

    pthread_mutex_lock(&manager->mutex);

    index = device_manager_find_index_locked(manager,node_id);

    if(index < 0 )
    {
        pthread_mutex_unlock(&manager->mutex);
        return DEVICE_MANAGER_NOT_FOUND;
    }

    manager->devices[index].online = online;

    /*
     *offline时统一保存-1
     *避免留下已经失效的fd
     */

     manager->devices[index].fd = online ? fd : -1;

     pthread_mutex_unlock(&manager->mutex);

     return DEVICE_MANAGER_OK;
}

int device_manager_update_last_seen(
    device_manager_t *manager,
    const char *node_id
)
{
    int index;
    time_t now;

    if(manager == NULL ||
        node_id == NULL ||
        node_id[0] == '\0')
    {
        return DEVICE_MANAGER_ERROR;
    }

    now = time(NULL);
    if(now == (time_t)-1)
    {
        return DEVICE_MANAGER_ERROR;
    }

    pthread_mutex_lock(&manager->mutex);

    index = device_manager_find_index_locked(manager,node_id);

    if(index < 0)
    {
        pthread_mutex_unlock(&manager->mutex);
        return DEVICE_MANAGER_NOT_FOUND;
    }

    manager->devices[index].last_seen = now;

    pthread_mutex_unlock(&manager->mutex);

    return DEVICE_MANAGER_OK;
}

int device_manager_bind_connection(
    device_manager_t *manager,
    const char *node_id,
    device_transport_t transport,
    int fd,
    pthread_t thread_id
)
{
    size_t ignored_count;


    return
        device_manager_bind_connection_collect_commands(
            manager,
            node_id,
            transport,
            fd,
            thread_id,
            NULL,
            0U,
            &ignored_count
        );
}

int device_manager_disconnect(
    device_manager_t *manager,
    const char *node_id,
    int fd,
    pthread_t thread_id
)
{
    size_t ignored_count;


    return
        device_manager_disconnect_collect_commands(
            manager,
            node_id,
            fd,
            thread_id,
            NULL,
            0U,
            &ignored_count
        );
}

int device_manager_disconnect_collect_commands(
    device_manager_t *manager,
    const char *node_id,
    int fd,
    pthread_t thread_id,
    frame_command_t *out_commands,
    size_t command_capacity,
    size_t *out_count
)
{
    int index;

    device_t *device;

    size_t pending_count;
    size_t i;

    if (manager == NULL ||
        node_id == NULL ||
        node_id[0] == '\0' ||
        fd < 0 ||
        out_count == NULL)
    {
        return DEVICE_MANAGER_ERROR;
    }


    if (out_commands == NULL &&
        command_capacity != 0U)
    {
        return DEVICE_MANAGER_ERROR;
    }


    *out_count = 0U;


    pthread_mutex_lock(
        &manager->mutex
    );


    index =
        device_manager_find_index_locked(
            manager,
            node_id
        );


    if (index < 0)
    {
        pthread_mutex_unlock(
            &manager->mutex
        );

        return DEVICE_MANAGER_NOT_FOUND;
    }

    device = &manager->devices[index];

    /*
     * 取出当前待发送命令数量。
     *
     * 必须在这里（进入所有权校验之前）读出来：
     * 后面既要用它做容量检查，也要用它做复制和清空，
     * 漏掉这一行会让 pending_count 是未初始化的栈垃圾。
     */
    pending_count = device->command_count;

    /*
     * 非常关键：
     *
     * 只有当前真正拥有这个连接的Worker
     * 才允许把设备标记offline并取走pending CMD。
     *
     * 防止旧Worker退出时误伤已经重连的新Worker。
     */
    if(device->fd != fd ||
        !pthread_equal(device->thread_id,
            thread_id))
    {
        pthread_mutex_unlock(&manager->mutex);

        return DEVICE_MANAGER_CONFLICT;
    }

    /*
     * caller提供了接收数组，但容量不够。
     *
     * 此时什么都不改：
     * 不offline、不清queue。
     */
    if(out_commands != NULL &&
        command_capacity < pending_count)
    {
        pthread_mutex_unlock(&manager->mutex);
        return DEVICE_MANAGER_BUFFER_TOO_SMALL;
    }

    /*
     *按FIFO顺序复制尚未发送的命令
     */
    if(out_commands != NULL)
    {
        for(i=0U; i<pending_count; i++)
        {
            size_t queue_index;

            queue_index = (device->command_head + i) %
            DEVICE_COMMAND_QUEUE_CAPACITY;

            out_commands[i] = device->command_queue[queue_index];
        }
    }

    /*
     *当前连接正式离线
     */
    device->online = 0;
    device->fd = -1;

    /*
     *queue里的命令已经交给caller
     *Device Manager不再持有它们
     */

    device->command_head = 0U;
    device ->command_tail = 0U;
    device->command_count = 0U;

    *out_count = pending_count;

    pthread_mutex_unlock(
        &manager->mutex
    );

    return DEVICE_MANAGER_OK;
}

/*
 * 向指定在线节点投递一条命令。
 *
 * 只做内存复制，不做任何socket/serial I/O。
 */
int device_manager_enqueue_command(
    device_manager_t *manager,
    const char *node_id,
    const frame_command_t *command
)
{
    int  index;
    device_t *device;

    if(manager == NULL ||
       node_id == NULL ||
       node_id[0] == '\0' ||
       command == NULL)
    {
        return DEVICE_MANAGER_ERROR;
    }

    //路由目标必须和命令本身记录的目标一致
    if(command->target_node[0] == '\0' ||
        strcmp(command->target_node,node_id) != 0)
    {
        return DEVICE_MANAGER_ERROR;
    }

    pthread_mutex_lock(&manager->mutex);

    index = device_manager_find_index_locked(manager,node_id);

    if(index < 0)
    {
        pthread_mutex_unlock(&manager->mutex);
        return DEVICE_MANAGER_NOT_FOUND;
    }

    device = &manager->devices[index];

    if(!device->online ||
        device->fd < 0)
    {
        pthread_mutex_unlock(&manager->mutex);
        return DEVICE_MANAGER_OFFLINE;
    }

    if(device->command_count >= DEVICE_COMMAND_QUEUE_CAPACITY)
    {
        pthread_mutex_unlock(&manager->mutex);

        return DEVICE_MANAGER_QUEUE_FULL;
    }

    /*
     *拷贝到tail位置
     */
    device->command_queue[device->command_tail] = *command;

    device->command_tail = (device->command_tail + 1U)%DEVICE_COMMAND_QUEUE_CAPACITY;

    device->command_count++;

    pthread_mutex_unlock(&manager->mutex);

    return DEVICE_MANAGER_OK;
}


/*
 * 当前设备Worker尝试取得一条自己的命令。
 *
 * fd + thread_id 用于确认调用者仍然是
 * Device Manager 当前记录的连接拥有者。
 */
int device_manager_try_dequeue_command(
    device_manager_t *manager,
    const char *node_id,
    int fd,
    pthread_t thread_id,
    frame_command_t *out_command
)
{
    int index;
    device_t *device;

    if (manager == NULL ||
        node_id == NULL ||
        node_id[0] == '\0' ||
        fd < 0 ||
        out_command == NULL)
    {
        return DEVICE_MANAGER_ERROR;
    }

    pthread_mutex_lock(
        &manager->mutex
    );

    index =
        device_manager_find_index_locked(
            manager,
            node_id
        );

    if (index < 0)
    {
        pthread_mutex_unlock(
            &manager->mutex
        );

        return DEVICE_MANAGER_NOT_FOUND;
    }

    device = &manager->devices[index];

    /*
     *必须仍然是当前连接
     *
     * 防止Worker读取新连接的命令
     */

     if(!device->online ||
        device->fd <0 ||
        pthread_equal(device->thread_id,
        thread_id) == 0)
    {
        pthread_mutex_unlock(&manager->mutex);

        return DEVICE_MANAGER_CONFLICT;
    }

    if(device->command_count == 0U)
    {
        pthread_mutex_unlock(&manager->mutex);
        return DEVICE_MANAGER_QUEUE_EMPTY;
    }

    *out_command = device->command_queue[device->command_head];

    device->command_head = (device->command_head + 1U)%DEVICE_COMMAND_QUEUE_CAPACITY;

    device->command_count --;

    pthread_mutex_unlock(&manager->mutex);

    return DEVICE_MANAGER_OK;
}

int device_manager_bind_connection_collect_commands(
    device_manager_t *manager,
    const char *node_id,
    device_transport_t transport,
    int fd,
    pthread_t thread_id,
    frame_command_t *out_commands,
    size_t command_capacity,
    size_t *out_count
)
{
    int index;

    device_t *device;

    size_t pending_count;
    size_t i;


    if (manager == NULL ||
        node_id == NULL ||
        node_id[0] == '\0' ||
        fd < 0 ||
        out_count == NULL)
    {
        return DEVICE_MANAGER_ERROR;
    }


    if (strlen(node_id) >=
        DEVICE_NODE_ID_SIZE)
    {
        return DEVICE_MANAGER_ERROR;
    }


    if (out_commands == NULL &&
        command_capacity != 0U)
    {
        return DEVICE_MANAGER_ERROR;
    }


    *out_count = 0U;


    pthread_mutex_lock(
        &manager->mutex
    );


    index =
        device_manager_find_index_locked(
            manager,
            node_id
        );


    /*
     * 第一次看到这个node。
     */
    if (index < 0)
    {
        if (manager->count >=
            DEVICE_MANAGER_MAX_DEVICES)
        {
            pthread_mutex_unlock(
                &manager->mutex
            );

            return DEVICE_MANAGER_FULL;
        }


        device =
            &manager->devices[
                manager->count
            ];


        memset(
            device,
            0,
            sizeof(*device)
        );


        strcpy(
            device->node_id,
            node_id
        );


        device->transport =
            transport;

        device->fd =
            fd;

        device->online =
            1;

        device->thread_id =
            thread_id;

        device->last_seen =
            time(NULL);


        /*
         * 新设备自然从空command queue开始。
         */
        device->command_head = 0U;
        device->command_tail = 0U;
        device->command_count = 0U;


        manager->count++;


        pthread_mutex_unlock(
            &manager->mutex
        );


        return DEVICE_MANAGER_OK;
    }


    device =
        &manager->devices[index];


    /*
     * 同一个node不能突然从Bluetooth变成WiFi，
     * 保持前面的transport冲突语义。
     */
    if (device->transport !=
        transport)
    {
        pthread_mutex_unlock(
            &manager->mutex
        );

        return DEVICE_MANAGER_CONFLICT;
    }


    /*
     * 非常重要：
     *
     * 如果本来就是当前这个连接再次bind，
     * 不允许清command queue。
     */
    if (device->online &&
        device->fd == fd &&
        pthread_equal(
            device->thread_id,
            thread_id
        ))
    {
        device->last_seen =
            time(NULL);


        pthread_mutex_unlock(
            &manager->mutex
        );


        return DEVICE_MANAGER_OK;
    }


    /*
     * 到这里意味着：
     *
     * 1. offline后的正常重连；
     * 或
     * 2. 新连接takeover旧连接。
     */
    pending_count =
        device->command_count;


    /*
     * caller想接收旧pending CMD，
     * 但buffer装不下。
     *
     * 此时必须保持所有状态不变。
     */
    if (out_commands != NULL &&
        command_capacity < pending_count)
    {
        pthread_mutex_unlock(
            &manager->mutex
        );

        return DEVICE_MANAGER_BUFFER_TOO_SMALL;
    }


    /*
     * 按FIFO顺序复制旧连接尚未发送的命令。
     */
    if (out_commands != NULL)
    {
        for (i = 0U;
             i < pending_count;
             i++)
        {
            size_t queue_index;


            queue_index =
                (
                    device->command_head +
                    i
                ) %
                DEVICE_COMMAND_QUEUE_CAPACITY;


            out_commands[i] =
                device->command_queue[
                    queue_index
                ];
        }
    }


    /*
     * 旧连接的queue生命周期到此结束。
     */
    device->command_head = 0U;
    device->command_tail = 0U;
    device->command_count = 0U;


    /*
     * 新连接成为当前owner。
     */
    device->fd =
        fd;

    device->online =
        1;

    device->thread_id =
        thread_id;

    device->last_seen =
        time(NULL);


    *out_count =
        pending_count;


    pthread_mutex_unlock(
        &manager->mutex
    );


    return DEVICE_MANAGER_OK;
}