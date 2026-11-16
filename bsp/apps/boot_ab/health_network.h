#ifndef SGW_HEALTH_NETWORK_H
#define SGW_HEALTH_NETWORK_H

#include "system_health.h"

/* Local interface only; external cloud availability is never inferred here. */
sgw_health_state_t sgw_health_network_probe(const char *interface_name);
sgw_health_state_t sgw_health_network_policy(void);

#endif
