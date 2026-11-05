
#include "ota_state.h"

#include <stdio.h>

/*
 * 不将有副作用的函数调用放进 assert()。
 * 即使使用 -DNDEBUG，也能正常运行测试。
 */
#define CHECK(expr) do {                           \
    if (!(expr)) {                                 \
        fprintf(stderr, "FAIL at line %d\n",      \
                __LINE__);                        \
        return 1;                                  \
    }                                              \
} while (0)

int main(void)
{
    ota_context_t ctx = { .state = OTA_IDLE };
    ota_state_t parsed;

    puts("=== Test 1: normal upgrade ===");

    CHECK(ota_state_transition(
        &ctx, OTA_DOWNLOADED) == 0);

    CHECK(ota_state_transition(
        &ctx, OTA_VERIFIED) == 0);

    CHECK(ota_state_transition(
        &ctx, OTA_STAGED) == 0);

    CHECK(ota_state_transition(
        &ctx, OTA_ACTIVATED) == 0);

    CHECK(ota_state_transition(
        &ctx, OTA_HEALTHY) == 0);

    CHECK(ota_state_transition(
        &ctx, OTA_CONFIRMED) == 0);

    CHECK(ctx.state == OTA_CONFIRMED);

    puts("=== Test 2: illegal transition ===");

    ctx.state = OTA_IDLE;

    CHECK(ota_state_transition(
        &ctx, OTA_ACTIVATED) == -1);

    CHECK(ctx.state == OTA_IDLE);

    puts("=== Test 3: interrupted upgrade ===");

    ctx.state = OTA_STAGED;

    CHECK(ota_state_transition(
        &ctx, OTA_ROLLBACK_PENDING) == 0);

    CHECK(ota_state_transition(
        &ctx, OTA_ROLLED_BACK) == 0);

    CHECK(ctx.state == OTA_ROLLED_BACK);

    puts("=== Test 4: state serialization ===");

    CHECK(ota_state_parse(
        "ACTIVATED", &parsed) == 0);

    CHECK(parsed == OTA_ACTIVATED);

    CHECK(ota_state_parse(
        "INVALID_STATE", &parsed) == -1);

    printf("Parsed state: %s\n",
           ota_state_name(OTA_ACTIVATED));

    puts("PASS: all OTA state module tests");

    return 0;
}
