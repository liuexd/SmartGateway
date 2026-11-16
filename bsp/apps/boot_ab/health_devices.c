#define _GNU_SOURCE
#include "health_devices.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define MAX_TEXT 1024

int sgw_health_parse_als(const char *text, unsigned int *value)
{
    unsigned long number;
    char *end;

    if (!text || !value || !isdigit((unsigned char)*text)) return -1;
    errno = 0;
    number = strtoul(text, &end, 10);
    if (errno || end == text || number > 65535) return -1;
    while (*end && isspace((unsigned char)*end)) ++end;
    if (*end) return -1;
    *value = (unsigned int)number;
    return 0;
}

sgw_health_state_t sgw_health_sensor_combine(sgw_health_state_t direct_read,
                                             sgw_health_state_t sensor_heartbeat)
{
    if (direct_read == SGW_HEALTH_UNHEALTHY ||
        sensor_heartbeat == SGW_HEALTH_UNHEALTHY) return SGW_HEALTH_UNHEALTHY;
    if (direct_read == SGW_HEALTH_HEALTHY &&
        sensor_heartbeat == SGW_HEALTH_HEALTHY) return SGW_HEALTH_HEALTHY;
    return SGW_HEALTH_UNKNOWN;
}

int sgw_health_ap3216_stats_valid(const char *text)
{
    static const char *keys[] = { "read_success=", "read_errors=" };
    const char *cursor = text;
    size_t i;
    if (!text) return 0;
    for (i = 0; i < 2; ++i) {
        char *end;
        size_t n = strlen(keys[i]);
        if (strncmp(cursor, keys[i], n) != 0) return 0;
        cursor += n;
        if (!isdigit((unsigned char)*cursor)) return 0;
        errno = 0;
        (void)strtoull(cursor, &end, 10);
        if (errno || *end != '\n') return 0;
        cursor = end + 1;
    }
    return *cursor == '\0';
}

int sgw_health_gpio_stats_valid(const char *text)
{
    static const char *keys[] = {
        "irq_total=", "enqueued=", "dropped=", "depth=", "capacity=",
        "head=", "tail=", "last_sequence=", "last_level=", "last_timestamp_ns="
    };
    unsigned long long values[10];
    const char *cursor = text;
    size_t i;

    if (!text) return 0;
    for (i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i) {
        size_t n = strlen(keys[i]);
        char *end;
        if (strncmp(cursor, keys[i], n) != 0) return 0;
        cursor += n;
        if (!isdigit((unsigned char)*cursor)) return 0;
        errno = 0;
        values[i] = strtoull(cursor, &end, 10);
        if (errno || end == cursor || *end != '\n' ||
            (i != 9 && values[i] > UINT_MAX)) return 0;
        cursor = end + 1;
    }
    if (*cursor || values[4] == 0 || values[3] > values[4] ||
        values[5] >= values[4] || values[6] >= values[4] || values[8] > 1)
        return 0;
    /* Zero IRQs is valid: the user may not have pressed the button. */
    return 1;
}

typedef struct {
    int error;
    int length;
    char text[MAX_TEXT];
} read_result_t;

static long long now_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return -1;
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* One-shot helper isolates potentially blocking sysfs reads. No event is read. */
static int bounded_read(const char *path, int timeout_ms, read_result_t *result)
{
    int fds[2], status;
    pid_t pid;
    long long start, deadline;
    size_t used = 0;
    char *out = (char *)result;

    if (!path || !result || timeout_ms <= 0 || timeout_ms > 5000) return -1;
    if (pipe2(fds, O_CLOEXEC | O_NONBLOCK) != 0) return -1;
    pid = fork();
    if (pid < 0) { close(fds[0]); close(fds[1]); return -1; }
    if (pid == 0) {
        read_result_t r;
        int fd;
        ssize_t count;
        memset(&r, 0, sizeof(r));
        close(fds[0]);
        fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
        if (fd < 0) {
            r.error = errno;
        } else {
            count = read(fd, r.text, sizeof(r.text) - 1);
            if (count < 0) r.error = errno;
            else if (count == 0 || count == (ssize_t)sizeof(r.text) - 1)
                r.error = EOVERFLOW;
            else { r.length = (int)count; r.text[count] = '\0'; }
            close(fd);
        }
        (void)write(fds[1], &r, sizeof(r));
        close(fds[1]);
        _exit(0);
    }
    close(fds[1]);
    start = now_ms();
    deadline = start + timeout_ms;
    memset(result, 0, sizeof(*result));
    while (start >= 0 && used < sizeof(*result)) {
        struct pollfd pfd = {fds[0], POLLIN, 0};
        long long now = now_ms();
        int polled;
        ssize_t n;
        if (now < 0 || now >= deadline) break;
        polled = poll(&pfd, 1, (int)(deadline - now));
        if (polled < 0 && errno == EINTR) continue;
        if (polled <= 0 || !(pfd.revents & POLLIN)) break;
        n = read(fds[0], out + used, sizeof(*result) - used);
        if (n < 0 && (errno == EINTR || errno == EAGAIN)) continue;
        if (n <= 0) break;
        used += (size_t)n;
    }
    close(fds[0]);
    if (used != sizeof(*result)) {
        (void)kill(pid, SIGKILL);
        (void)waitpid(pid, &status, WNOHANG);
        return -1;
    }
    (void)waitpid(pid, &status, WNOHANG);
    return 0;
}

int sgw_health_read_text_bounded(const char *path, int timeout_ms,
                                 char *out, size_t out_size, int *read_error)
{
    read_result_t result;
    if (!out || out_size == 0 || !read_error) return -1;
    out[0] = '\0';
    *read_error = 0;
    if (bounded_read(path, timeout_ms, &result) != 0) return -1;
    if (result.error) {
        *read_error = result.error;
        return 0;
    }
    if (result.length <= 0 || (size_t)result.length >= out_size) return -1;
    memcpy(out, result.text, (size_t)result.length + 1);
    return 0;
}

static sgw_health_state_t failure_state(int err)
{
    if (err == ENOENT || err == ENODEV || err == ENXIO || err == EIO ||
        err == EREMOTEIO) return SGW_HEALTH_UNHEALTHY;
    return SGW_HEALTH_UNKNOWN;
}

static int bound_driver(const char *link, const char *driver)
{
    char resolved[PATH_MAX], *base;
    if (!realpath(link, resolved)) return -1;
    base = strrchr(resolved, '/');
    return base && strcmp(base + 1, driver) == 0 ? 1 : 0;
}

sgw_health_state_t sgw_health_ap3216_read(unsigned int *raw, int timeout_ms)
{
    read_result_t result;
    int driver = bound_driver("/sys/bus/i2c/devices/0-001e/driver", "sgw_ap3216");
    if (driver < 0) return failure_state(errno);
    if (driver == 0) return SGW_HEALTH_UNHEALTHY;
    if (bounded_read("/sys/bus/i2c/devices/0-001e/als_raw", timeout_ms, &result) != 0)
        return SGW_HEALTH_UNKNOWN;
    if (result.error) return failure_state(result.error);
    if (sgw_health_parse_als(result.text, raw) != 0) return SGW_HEALTH_UNKNOWN;
    if (bounded_read("/sys/bus/i2c/devices/0-001e/stats", timeout_ms, &result) != 0)
        return SGW_HEALTH_UNKNOWN;
    if (result.error) return failure_state(result.error);
    if (!sgw_health_ap3216_stats_valid(result.text)) return SGW_HEALTH_UNKNOWN;
    return SGW_HEALTH_HEALTHY;
}

sgw_health_state_t sgw_health_gpio_inventory(int timeout_ms)
{
    struct stat st;
    read_result_t dev, stats;
    unsigned int maj, min;
    int used = 0, driver;

    if (stat("/dev/sgw_event0", &st) != 0) return failure_state(errno);
    if (!S_ISCHR(st.st_mode)) return SGW_HEALTH_UNHEALTHY;
    driver = bound_driver("/sys/bus/platform/devices/sgw-gpio-event/driver",
                          "sgw-gpio-event");
    if (driver < 0) return failure_state(errno);
    if (driver == 0) return SGW_HEALTH_UNHEALTHY;
    if (bounded_read("/sys/class/misc/sgw_event0/dev", timeout_ms, &dev) != 0)
        return SGW_HEALTH_UNKNOWN;
    if (dev.error) return failure_state(dev.error);
    if (sscanf(dev.text, "%u:%u\n%n", &maj, &min, &used) != 2 || dev.text[used] ||
        maj != major(st.st_rdev) || min != minor(st.st_rdev))
        return SGW_HEALTH_UNHEALTHY;
    if (bounded_read("/sys/bus/platform/devices/sgw-gpio-event/stats",
                     timeout_ms, &stats) != 0)
        return SGW_HEALTH_UNKNOWN;
    if (stats.error) return failure_state(stats.error);
    if (!sgw_health_gpio_stats_valid(stats.text)) return SGW_HEALTH_UNKNOWN;
    return SGW_HEALTH_HEALTHY;
}
