#define _POSIX_C_SOURCE 200809L
#include "health_devices.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static int checks, failures;
#define CHECK(x) do { checks++; if (!(x)) { \
    fprintf(stderr, "FAIL:%d: %s\n", __LINE__, #x); failures++; } } while (0)

static long long ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return -1;
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int main(void)
{
    unsigned int raw = 999;
    const char *zero =
        "irq_total=0\nenqueued=0\ndropped=0\ndepth=0\ncapacity=32\n"
        "head=0\ntail=0\nlast_sequence=0\nlast_level=0\nlast_timestamp_ns=0\n";
    const char *bad =
        "irq_total=0\nenqueued=0\ndropped=0\ndepth=33\ncapacity=32\n"
        "head=0\ntail=0\nlast_sequence=0\nlast_level=0\nlast_timestamp_ns=0\n";
    char dir[] = "/tmp/sgw-devices-XXXXXX", fifo[256], buf[128];
    long long before, elapsed;
    int read_error = 0;

    CHECK(sgw_health_parse_als("0\n", &raw) == 0 && raw == 0);
    CHECK(sgw_health_parse_als("65535\n", &raw) == 0 && raw == 65535);
    CHECK(sgw_health_parse_als("65536\n", &raw) != 0);
    CHECK(sgw_health_parse_als("-1\n", &raw) != 0);
    CHECK(sgw_health_parse_als("0\ninvalid", &raw) != 0);
    CHECK(sgw_health_parse_als("", &raw) != 0);
    CHECK(sgw_health_ap3216_stats_valid("read_success=0\nread_errors=3\n"));
    CHECK(!sgw_health_ap3216_stats_valid("read_success=1\nread_errors=-1\n"));
    CHECK(!sgw_health_ap3216_stats_valid("read_success=1\n"));
    /* SENSOR=ERROR overrides a successful ALS read, regardless of ONLINE. */
    CHECK(sgw_health_sensor_combine(SGW_HEALTH_HEALTHY, SGW_HEALTH_UNHEALTHY) ==
          SGW_HEALTH_UNHEALTHY);
    CHECK(sgw_health_sensor_combine(SGW_HEALTH_UNHEALTHY, SGW_HEALTH_HEALTHY) ==
          SGW_HEALTH_UNHEALTHY);
    CHECK(sgw_health_sensor_combine(SGW_HEALTH_HEALTHY, SGW_HEALTH_UNKNOWN) ==
          SGW_HEALTH_UNKNOWN);
    CHECK(sgw_health_sensor_combine(SGW_HEALTH_HEALTHY, SGW_HEALTH_HEALTHY) ==
          SGW_HEALTH_HEALTHY);
    CHECK(sgw_health_gpio_stats_valid(zero)); /* no button press is normal */
    CHECK(!sgw_health_gpio_stats_valid(bad));
    CHECK(!sgw_health_gpio_stats_valid("irq_total=0\n"));
    CHECK(!sgw_health_gpio_stats_valid(
        "irq_total=-1\nenqueued=0\ndropped=0\ndepth=0\ncapacity=32\n"
        "head=0\ntail=0\nlast_sequence=0\nlast_level=0\nlast_timestamp_ns=0\n"));
    CHECK(mkdtemp(dir) != NULL);
    snprintf(fifo, sizeof(fifo), "%s/stalled", dir);
    CHECK(mkfifo(fifo, 0600) == 0);
    before = ms();
    CHECK(sgw_health_read_text_bounded(fifo, 150, buf, sizeof(buf), &read_error) != 0);
    elapsed = ms() - before;
    CHECK(elapsed >= 100 && elapsed < 1500);
    unlink(fifo);
    rmdir(dir);
    printf("health_devices_test: checks=%d failures=%d\n", checks, failures);
    return failures != 0;
}
