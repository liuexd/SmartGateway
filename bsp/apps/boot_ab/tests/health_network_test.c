#include "health_network.h"
#include <stdio.h>

static int checks, failures;
#define CHECK(x) do { checks++; if (!(x)) { \
    fprintf(stderr, "FAIL:%d: %s\n", __LINE__, #x); failures++; } } while (0)

int main(void)
{
    CHECK(sgw_health_network_probe(NULL) == SGW_HEALTH_UNKNOWN);
    CHECK(sgw_health_network_probe("") == SGW_HEALTH_UNKNOWN);
    CHECK(sgw_health_network_probe("bad/name") == SGW_HEALTH_UNKNOWN);
    CHECK(sgw_health_network_probe("lo") == SGW_HEALTH_UNHEALTHY);
    CHECK(sgw_health_network_probe("sgw-no-such0") == SGW_HEALTH_UNHEALTHY);
    printf("health_network_test: checks=%d failures=%d\n", checks, failures);
    return failures != 0;
}
