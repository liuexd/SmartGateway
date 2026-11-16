#define _POSIX_C_SOURCE 200809L
#include "ab_state.h"
#include "ab_health.h"
#include "fw_env_backend.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/watchdog.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static uint64_t monotonic_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000ULL);
}

static void sleep_ms(uint64_t ms)
{
    struct timespec req;
    req.tv_sec = (time_t)(ms / 1000ULL);
    req.tv_nsec = (long)((ms % 1000ULL) * 1000000ULL);
    while (nanosleep(&req, &req) < 0 && errno == EINTR) {
    }
}

static int wdt_kick(int fd)
{
    int dummy = 0;
    if (ioctl(fd, WDIOC_KEEPALIVE, &dummy) < 0 && errno != ENOTTY) {
        return -1;
    }
    if (write(fd, "k", 1) != 1) {
        return -1;
    }
    return 0;
}

static void print_usage(const char *prog)
{
    fprintf(stderr,
            "Usage: %s [-c fw_env.config] [--wdt-dev /dev/watchdog] "
            "[--timeout-s 60] [--budget-ms 45000] [--poll-ms 1000] "
            "[--run-dir dir] [--cmdline path] [--mounts path] [--sysroot dir] "
            "[--exit-on-confirm]\n",
            prog);
}

int main(int argc, char **argv)
{
    fw_env_backend_cfg_t cfg;
    ab_health_cfg_t hcfg;
    ab_env_t env;
    char errbuf[256] = {0};
    const char *wdt_dev = "/dev/watchdog";
    int timeout_s = 60;
    uint64_t budget_ms = 45000;
    uint64_t poll_ms = 1000;
    int exit_on_confirm = 0;
    int wdt_fd = -1;
    int timeout_arg;
    uint64_t start_ms;

    fw_env_backend_cfg_from_env(&cfg);
    ab_health_cfg_init(&hcfg);

    for (int i = 1; i < argc; i++) {
        if ((strcmp(argv[i], "-c") == 0 || strcmp(argv[i], "--config") == 0) && i + 1 < argc) {
            cfg.config_path = argv[++i];
        } else if (strcmp(argv[i], "--wdt-dev") == 0 && i + 1 < argc) {
            wdt_dev = argv[++i];
        } else if (strcmp(argv[i], "--timeout-s") == 0 && i + 1 < argc) {
            timeout_s = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--budget-ms") == 0 && i + 1 < argc) {
            budget_ms = strtoull(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--poll-ms") == 0 && i + 1 < argc) {
            poll_ms = strtoull(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--run-dir") == 0 && i + 1 < argc) {
            hcfg.supervisor_run_dir = argv[++i];
        } else if (strcmp(argv[i], "--cmdline") == 0 && i + 1 < argc) {
            hcfg.proc_cmdline_path = argv[++i];
        } else if (strcmp(argv[i], "--mounts") == 0 && i + 1 < argc) {
            hcfg.proc_mounts_path = argv[++i];
        } else if (strcmp(argv[i], "--sysroot") == 0 && i + 1 < argc) {
            hcfg.sysroot_prefix = argv[++i];
        } else if (strcmp(argv[i], "--exit-on-confirm") == 0) {
            exit_on_confirm = 1;
        } else {
            print_usage(argv[0]);
            return 2;
        }
    }

    if (fw_env_backend_load(&cfg, &env, errbuf, sizeof(errbuf)) != 0) {
        fprintf(stderr, "ERROR loading fw_env: %s\n", errbuf);
        return 1;
    }

    if (env.ab_state != AB_STATE_TRIAL && env.upgrade_available == 0) {
        printf("STEADY state=%s active=%s: trial guard not active\n",
               ab_state_str(env.ab_state), ab_slot_str(env.active_slot));
        return 0;
    }

    /* Take ownership of /dev/watchdog immediately (stops Linux kernel auto-worker). */
    wdt_fd = open(wdt_dev, O_WRONLY);
    if (wdt_fd < 0) {
        fprintf(stderr, "ERROR opening watchdog '%s': %s\n", wdt_dev, strerror(errno));
        return 1;
    }

    timeout_arg = timeout_s;
    if (ioctl(wdt_fd, WDIOC_SETTIMEOUT, &timeout_arg) < 0 && errno != ENOTTY) {
        fprintf(stderr, "WARN: WDIOC_SETTIMEOUT failed: %s\n", strerror(errno));
    }

    printf("WATCHDOG_TAKEOVER dev=%s timeout_s=%d budget_ms=%llu bootcount=%lu/%lu\n",
           wdt_dev, timeout_arg, (unsigned long long)budget_ms,
           env.bootcount, env.bootlimit);
    fflush(stdout);

    start_ms = monotonic_ms();
    while (1) {
        ab_slot_t detected_running = AB_SLOT_NONE;
        unsigned long long svc_pid = 0;
        uint64_t elapsed = monotonic_ms() - start_ms;

        if (wdt_kick(wdt_fd) != 0) {
            fprintf(stderr, "ERROR feeding watchdog: %s\n", strerror(errno));
            close(wdt_fd); /* intentionally NO 'V' magic close */
            return 1;
        }

        if (ab_verify_system_health(&hcfg, &env, &detected_running, &svc_pid,
                                    errbuf, sizeof(errbuf)) == 0) {
            if (ab_confirm_trial(&env, detected_running, 1, errbuf, sizeof(errbuf)) == 0 &&
                fw_env_backend_save(&cfg, &env, errbuf, sizeof(errbuf)) == 0) {
                printf("TRIAL_CONFIRMED slot=%s service_pid=%llu elapsed_ms=%llu\n",
                       ab_slot_str(detected_running), svc_pid,
                       (unsigned long long)elapsed);
                fflush(stdout);

                if (exit_on_confirm) {
                    /* Magic close 'V' only after confirmed steady state */
                    if (write(wdt_fd, "V", 1) < 0) {
                        perror("write magic close");
                    }
                    close(wdt_fd);
                    return 0;
                }

                /* Continue feeding watchdog cleanly in steady state */
                while (1) {
                    sleep_ms(poll_ms);
                    if (wdt_kick(wdt_fd) != 0) {
                        close(wdt_fd);
                        return 1;
                    }
                }
            }
        }

        if (ab_watchdog_trial_expired(elapsed, budget_ms, 0)) {
            snprintf(env.last_error, sizeof(env.last_error), "%s", "health_timeout");
            (void)fw_env_backend_save(&cfg, &env, errbuf, sizeof(errbuf));
            fprintf(stderr,
                    "WATCHDOG_EXPIRED elapsed_ms=%llu budget_ms=%llu reason=%s "
                    "(stopping keepalive without magic close)\n",
                    (unsigned long long)elapsed, (unsigned long long)budget_ms,
                    errbuf[0] ? errbuf : "health_timeout");
            /* Deliberately close WITHOUT 'V' magic close so WDOG1 resets SoC */
            close(wdt_fd);
            return 3;
        }

        sleep_ms(poll_ms);
    }
}
