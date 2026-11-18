#ifndef SGW_GATEWAY_HEALTH_PROBE_H
#define SGW_GATEWAY_HEALTH_PROBE_H

#include "device_manager.h"
#include "message_json.h"
#include "message_queue.h"
#include <stdint.h>

/* Local-only read-only diagnostic, serviced by Gateway's health thread. */
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
                                     device_manager_t *devices,
                                     uint16_t wifi_port);
void gateway_health_listener_close(gateway_health_listener_t *listener);

/* Diagnostic only: a successful transport send is not a remote receipt. */
void gateway_health_observe_forwarded(const unsigned char *data, size_t length);
/* Diagnostic only: server_link loop heartbeat (not external receipt). */
void gateway_health_observe_worker_tick(void);
/* Check that device_manager mutex/count is healthy without mutating devices. */
int gateway_health_device_manager_ok(device_manager_t *devices);
/* Verify incoming _DIAG frame matches an active pending SGW-LOCAL/1 probe. */
int gateway_health_check_pending_diag(const frame_data_t *data);
/* Non-blocking wrap and enqueue of a local-only marker from gateway_app. */
int gateway_health_try_enqueue_local_marker(message_queue_t *queue,
                                            const char *json,
                                            size_t json_len);
/* Returns 1 and consumes marker if queue item is a local-only probe (never sent upstream). */
int gateway_health_consume_local_marker(const unsigned char *data, size_t length);
/* When an ordinary message is awaiting reconnect, remove only a local probe
 * from the queue; preserve the order and contents of every business item. */
int gateway_health_consume_queued_marker(message_queue_t *queue);

#endif
