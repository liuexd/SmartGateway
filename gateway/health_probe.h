#ifndef SGW_GATEWAY_HEALTH_PROBE_H
#define SGW_GATEWAY_HEALTH_PROBE_H

#include "device_manager.h"
#include "message_queue.h"

/* Local-only read-only diagnostic, serviced by Gateway's existing main loop. */
typedef struct {
    int fd;
    char path[108];
    unsigned long long socket_device;
    unsigned long long socket_inode;
} gateway_health_listener_t;

void gateway_health_listener_init(gateway_health_listener_t *listener);
int gateway_health_listener_open(gateway_health_listener_t *listener);
void gateway_health_listener_service(gateway_health_listener_t *listener,
                                     message_queue_t *queue,
                                     device_manager_t *devices);
void gateway_health_listener_close(gateway_health_listener_t *listener);

/* Diagnostic only: a successful transport send is not a remote receipt. */
void gateway_health_observe_forwarded(const unsigned char *data, size_t length);
/* Diagnostic only: server_link loop heartbeat (not external receipt). */
void gateway_health_observe_worker_tick(void);
/* Returns 1 and consumes marker if queue item is a local-only probe (never sent upstream). */
int gateway_health_consume_local_marker(const unsigned char *data, size_t length);
/* When an ordinary message is awaiting reconnect, remove only a local probe
 * from the queue; preserve the order and contents of every business item. */
int gateway_health_consume_queued_marker(message_queue_t *queue);

#endif
