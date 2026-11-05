
#include "ota_state.h"

#include <stddef.h>
#include <string.h>

static const char *const state_names[] = {
    "IDLE",
    "DOWNLOADED",
    "VERIFIED",
    "STAGED",
    "ACTIVATED",
    "HEALTHY",
    "CONFIRMED",
    "ROLLBACK_PENDING",
    "ROLLED_BACK"
};

static int state_valid(ota_state_t state)
{
    return state >= OTA_IDLE &&
           state < OTA_STATE_COUNT;
}

const char *ota_state_name(ota_state_t state)
{
    if (!state_valid(state))
        return "UNKNOWN";

    return state_names[state];
}

int ota_state_parse(
    const char *name,
    ota_state_t *out_state)
{
    if (name == NULL || out_state == NULL)
        return -1;

    for (int i = 0; i < OTA_STATE_COUNT; i++) {
        if (strcmp(name, state_names[i]) == 0) {
            *out_state = (ota_state_t)i;
            return 0;
        }
    }

    return -1;
}

int ota_state_can_transition(
    ota_state_t from,
    ota_state_t to)
{
    if (!state_valid(from) || !state_valid(to))
        return 0;

    switch (from) {
    case OTA_IDLE:
        return to == OTA_DOWNLOADED;

    case OTA_DOWNLOADED:
        return to == OTA_VERIFIED ||
            to == OTA_ROLLBACK_PENDING;

    case OTA_VERIFIED:
        return to == OTA_STAGED ||
            to == OTA_ROLLBACK_PENDING;

    case OTA_STAGED:
        return to == OTA_ACTIVATED ||
               to == OTA_ROLLBACK_PENDING;

    case OTA_ACTIVATED:
        return to == OTA_HEALTHY ||
               to == OTA_ROLLBACK_PENDING;

    case OTA_HEALTHY:
        return to == OTA_CONFIRMED ||
               to == OTA_ROLLBACK_PENDING;

    case OTA_ROLLBACK_PENDING:
        return to == OTA_ROLLED_BACK;

    case OTA_CONFIRMED:
    case OTA_ROLLED_BACK:
        return to == OTA_DOWNLOADED;
    default:
        return 0;
    }
}

int ota_state_transition(
    ota_context_t *ctx,
    ota_state_t next)
{
    if (ctx == NULL)
        return -1;

    if (!ota_state_can_transition(ctx->state, next))
        return -1;

    ctx->state = next;
    return 0;
}
