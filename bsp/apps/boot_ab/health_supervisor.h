#ifndef SGW_HEALTH_SUPERVISOR_H
#define SGW_HEALTH_SUPERVISOR_H

#include <sys/types.h>

/*
 * STATUS is process evidence only, never a business-health attestation.
 * The caller supplies trusted paths/owner from its compiled policy.
 * 0: verified managed live process; -1: failed or unverified.
 */
int sgw_health_supervisor_status(const char *socket_path,
                                 const char *supervisor_executable,
                                 const char *gateway_executable,
                                 uid_t trusted_uid,
                                 int timeout_ms,
                                 pid_t *gateway_pid);

#endif
