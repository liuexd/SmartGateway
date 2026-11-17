#ifndef SGW_HEALTH_DEVICES_H
#define SGW_HEALTH_DEVICES_H

#include "system_health.h"
#include <stddef.h>

int sgw_health_parse_als(const char *text, unsigned int *value);
int sgw_health_ap3216_stats_valid(const char *text);
sgw_health_state_t sgw_health_sensor_combine(sgw_health_state_t direct_read,
                                             sgw_health_state_t sensor_heartbeat);
int sgw_health_gpio_stats_valid(const char *text);
int sgw_health_read_text_bounded(const char *path, int timeout_ms,
                                 char *out, size_t out_size, int *read_error);

/* Read-only diagnostics; they never read the GPIO event queue. */
sgw_health_state_t sgw_health_ap3216_read(unsigned int *raw, int timeout_ms);
sgw_health_state_t sgw_health_ap3216_read_at(const char *als_path,
                                             const char *stats_path,
                                             const char *driver_link,
                                             unsigned int *raw,
                                             int timeout_ms);
sgw_health_state_t sgw_health_gpio_inventory(int timeout_ms);
sgw_health_state_t sgw_health_gpio_inventory_at(const char *dev_node,
                                                const char *misc_dev_path,
                                                const char *stats_path,
                                                const char *driver_link,
                                                int timeout_ms);

#endif
