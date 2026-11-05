
#include <assert.h>
#include <stdio.h>

typedef enum {
    OTA_IDLE = 0,
    OTA_DOWNLOADED,
    OTA_VERIFIED,
    OTA_STAGED,
    OTA_ACTIVATED,
    OTA_HEALTHY,
    OTA_CONFIRMED,
    OTA_ROLLBACK_PENDING,
    OTA_ROLLED_BACK
} ota_state_t;

typedef struct {
    ota_state_t state;
} ota_context_t;

static const char *state_name(ota_state_t state)
{
    switch (state) {
    case OTA_IDLE:             return "IDLE";
    case OTA_DOWNLOADED:       return "DOWNLOADED";
    case OTA_VERIFIED:         return "VERIFIED";
    case OTA_STAGED:           return "STAGED";
    case OTA_ACTIVATED:        return "ACTIVATED";
    case OTA_HEALTHY:          return "HEALTHY";
    case OTA_CONFIRMED:        return "CONFIRMED";
    case OTA_ROLLBACK_PENDING: return "ROLLBACK_PENDING";
    case OTA_ROLLED_BACK:      return "ROLLED_BACK";
    default:                   return "UNKNOWN";
    }
}

/* 返回 1 表示允许跳转，0 表示禁止 */
static int transition_allowed(
    ota_state_t from,
    ota_state_t to)
{
    switch (from) {
    case OTA_IDLE:
        return to == OTA_DOWNLOADED;

    case OTA_DOWNLOADED:
        return to == OTA_VERIFIED;

    case OTA_VERIFIED:
        return to == OTA_STAGED;

    case OTA_STAGED:
        return to == OTA_ACTIVATED;

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
    default:
        return 0;
    }
}

static int ota_transition(
    ota_context_t *ctx,
    ota_state_t next)
{
    if (!transition_allowed(ctx->state, next)) {
        printf("REJECT: %s -> %s\n",
               state_name(ctx->state),
               state_name(next));
        return -1;
    }

    printf("OK: %s -> %s\n",
           state_name(ctx->state),
           state_name(next));

    ctx->state = next;
    return 0;
}

int main(void)
{
    ota_context_t success = { .state = OTA_IDLE };

    puts("=== Test 1: normal upgrade ===");

    assert(ota_transition(&success, OTA_DOWNLOADED) == 0);
    assert(ota_transition(&success, OTA_VERIFIED) == 0);
    assert(ota_transition(&success, OTA_STAGED) == 0);
    assert(ota_transition(&success, OTA_ACTIVATED) == 0);
    assert(ota_transition(&success, OTA_HEALTHY) == 0);
    assert(ota_transition(&success, OTA_CONFIRMED) == 0);

    assert(success.state == OTA_CONFIRMED);

    puts("\n=== Test 2: illegal transition ===");

    ota_context_t invalid = { .state = OTA_IDLE };

    assert(ota_transition(&invalid, OTA_ACTIVATED) == -1);
    assert(invalid.state == OTA_IDLE);

    puts("\n=== Test 3: rollback ===");

    ota_context_t failed = { .state = OTA_STAGED };

    assert(ota_transition(&failed, OTA_ACTIVATED) == 0);
    assert(ota_transition(&failed, OTA_ROLLBACK_PENDING) == 0);
    assert(ota_transition(&failed, OTA_ROLLED_BACK) == 0);

    assert(failed.state == OTA_ROLLED_BACK);

    puts("\nPASS: all OTA state machine tests");
    return 0;
}
