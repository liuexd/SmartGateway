#ifndef SGW_HEALTH_GATEWAY_H
#define SGW_HEALTH_GATEWAY_H
#include <sys/types.h>

/* Tests actual Gateway main-loop parser/queue diagnostic, not remote delivery. */
int sgw_health_gateway_probe(const char *socket_path, uid_t trusted_uid,
                             pid_t expected_pid, unsigned long long expected_start,
                             int timeout_ms);
/* Recent actual DATA send by the Gateway worker, not end-to-end receipt. */
int sgw_health_gateway_flow_probe(const char *socket_path, uid_t trusted_uid,
                                  pid_t expected_pid, unsigned long long expected_start,
                                  int timeout_ms);
/* Controlled local loop + server_link worker liveness (no upstream send). */
int sgw_health_gateway_local_probe(const char *socket_path, uid_t trusted_uid,
                                   pid_t expected_pid, unsigned long long expected_start,
                                   int timeout_ms);
#endif
