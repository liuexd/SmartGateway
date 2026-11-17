#define _GNU_SOURCE
#include "r3c_policy.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int fail(char *err, size_t len, const char *msg)
{
    if (err && len) snprintf(err, len, "%s", msg);
    return -1;
}

static int safe_abs(const char *s)
{
    size_t n;
    if (!s || s[0] != '/' || s[1] == '\0') return 0;
    n = strlen(s);
    if (n >= PATH_MAX || s[n - 1] == '/' || strstr(s, "//") ||
        strstr(s, "/../") || strstr(s, "/./") || strpbrk(s, " \t\r\n\\"))
        return 0;
    if (n >= 3 && strcmp(s + n - 3, "/..") == 0) return 0;
    if (n >= 2 && strcmp(s + n - 2, "/.") == 0) return 0;
    return 1;
}

static int trusted_dirs(const char *path, uid_t owner)
{
    char buf[PATH_MAX];
    struct stat st;
    size_t i, n;
    if (!safe_abs(path)) return -1;
    n = strlen(path);
    memcpy(buf, path, n + 1);
    if (lstat("/", &st) != 0 || st.st_uid != 0 || (st.st_mode & 0022)) return -1;
    for (i = 1; i <= n; ++i) {
        char saved;
        if (buf[i] != '/' && buf[i] != '\0') continue;
        saved = buf[i];
        buf[i] = '\0';
        if (saved == '\0') break;
        if (lstat(buf, &st) != 0 || !S_ISDIR(st.st_mode) ||
            (st.st_uid != 0 && st.st_uid != owner) || (st.st_mode & 0022))
            return -1;
        buf[i] = saved;
    }
    return 0;
}

void r3c_policy_init_default(r3c_policy_t *p, uid_t owner)
{
    if (!p) return;
    memset(p, 0, sizeof(*p));
    p->trusted_uid = owner;
    snprintf(p->data_mountpoint, sizeof(p->data_mountpoint), "/opt/smartgateway/data");
    snprintf(p->data_device, sizeof(p->data_device), "/dev/mmcblk1p3");
    p->data_min_bytes = 16ULL * 1024 * 1024;
    snprintf(p->app_ota_dir, sizeof(p->app_ota_dir), "/opt/smartgateway");
    snprintf(p->tx_dir, sizeof(p->tx_dir), "/opt/smartgateway/data/.sgw-tx");
    snprintf(p->pubkey_path, sizeof(p->pubkey_path), "/etc/smartgateway/ota-public.pem");
    snprintf(p->slot1_device, sizeof(p->slot1_device), "/dev/mmcblk1p1");
    snprintf(p->slot2_device, sizeof(p->slot2_device), "/dev/mmcblk1p2");
    snprintf(p->whole_disk_device, sizeof(p->whole_disk_device), "/dev/mmcblk1");
    snprintf(p->supervisor_sock, sizeof(p->supervisor_sock), "/opt/smartgateway/run/supervisor.sock");
    snprintf(p->supervisor_exe, sizeof(p->supervisor_exe), "/opt/smartgateway/bin/service_supervisor");
    snprintf(p->gateway_sock, sizeof(p->gateway_sock), "/opt/smartgateway/run/gateway-health.sock");
    snprintf(p->gateway_exe, sizeof(p->gateway_exe), "/opt/smartgateway/current/smart_gateway");
    snprintf(p->net_iface, sizeof(p->net_iface), "eth0");
    /* Fail-closed defaults: require actual forward observation & disallow degraded confirm */
    p->gateway_flow_mode = R3C_GW_STRICT_FORWARD;
    p->allow_external_degraded = 0;
    p->require_sensor_heartbeat = 1;
    p->stable_ms = 5000;
    p->max_gap_ms = 2000;
    p->deadline_ms = 30000;
    p->max_evidence_age_ms = 3000;
}

static int parse_u64(const char *s, uint64_t *out)
{
    char *end = NULL;
    unsigned long long v;
    if (!s || !s[0]) return -1;
    errno = 0;
    v = strtoull(s, &end, 10);
    if (errno || end == s || *end != '\0') return -1;
    *out = (uint64_t)v;
    return 0;
}

int r3c_policy_load(const char *path, uid_t expected_owner,
                    r3c_policy_t *out, char *err, size_t errlen)
{
    char buf[8193];
    struct stat st;
    int fd = -1;
    ssize_t n;
    char *line, *save = NULL;
    unsigned seen_mask = 0;

    if (!path || !out) return fail(err, errlen, "null policy argument");
    if (trusted_dirs(path, expected_owner) != 0)
        return fail(err, errlen, "untrusted policy path");
    fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) return fail(err, errlen, "missing R3C policy file");
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_uid != expected_owner ||
        (st.st_mode & 0022) || st.st_nlink != 1 || st.st_size <= 0 || st.st_size > 8192) {
        close(fd);
        return fail(err, errlen, "untrusted policy file permissions or size");
    }
    n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0 || n != st.st_size || memchr(buf, '\0', (size_t)n))
        return fail(err, errlen, "cannot read policy file");
    buf[n] = '\0';

    r3c_policy_init_default(out, expected_owner);
    for (line = strtok_r(buf, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        char *eq;
        const char *k, *v;
        size_t len = strlen(line);
        if (len > 0 && line[len - 1] == '\r') line[--len] = '\0';
        if (line[0] == '\0' || line[0] == '#') continue;
        eq = strchr(line, '=');
        if (!eq || eq == line) return fail(err, errlen, "malformed policy line");
        *eq = '\0';
        k = line;
        v = eq + 1;
        if (strcmp(k, "data_mountpoint") == 0) {
            if ((seen_mask & (1u << 0)) || !safe_abs(v)) return fail(err, errlen, "invalid data_mountpoint");
            seen_mask |= (1u << 0);
            snprintf(out->data_mountpoint, sizeof(out->data_mountpoint), "%s", v);
        } else if (strcmp(k, "data_device") == 0) {
            if ((seen_mask & (1u << 1)) || !safe_abs(v)) return fail(err, errlen, "invalid data_device");
            seen_mask |= (1u << 1);
            snprintf(out->data_device, sizeof(out->data_device), "%s", v);
        } else if (strcmp(k, "data_min_bytes") == 0) {
            uint64_t val = 0;
            if ((seen_mask & (1u << 2)) || parse_u64(v, &val) != 0 || val == 0)
                return fail(err, errlen, "invalid data_min_bytes");
            seen_mask |= (1u << 2);
            out->data_min_bytes = val;
        } else if (strcmp(k, "app_ota_dir") == 0) {
            if ((seen_mask & (1u << 3)) || !safe_abs(v)) return fail(err, errlen, "invalid app_ota_dir");
            seen_mask |= (1u << 3);
            snprintf(out->app_ota_dir, sizeof(out->app_ota_dir), "%s", v);
        } else if (strcmp(k, "tx_dir") == 0) {
            if ((seen_mask & (1u << 4)) || !safe_abs(v)) return fail(err, errlen, "invalid tx_dir");
            seen_mask |= (1u << 4);
            snprintf(out->tx_dir, sizeof(out->tx_dir), "%s", v);
        } else if (strcmp(k, "pubkey_path") == 0) {
            if ((seen_mask & (1u << 5)) || !safe_abs(v)) return fail(err, errlen, "invalid pubkey_path");
            seen_mask |= (1u << 5);
            snprintf(out->pubkey_path, sizeof(out->pubkey_path), "%s", v);
        } else if (strcmp(k, "slot1_device") == 0) {
            if ((seen_mask & (1u << 6)) || !safe_abs(v)) return fail(err, errlen, "invalid slot1_device");
            seen_mask |= (1u << 6);
            snprintf(out->slot1_device, sizeof(out->slot1_device), "%s", v);
        } else if (strcmp(k, "slot2_device") == 0) {
            if ((seen_mask & (1u << 7)) || !safe_abs(v)) return fail(err, errlen, "invalid slot2_device");
            seen_mask |= (1u << 7);
            snprintf(out->slot2_device, sizeof(out->slot2_device), "%s", v);
        } else if (strcmp(k, "whole_disk_device") == 0) {
            if ((seen_mask & (1u << 8)) || !safe_abs(v)) return fail(err, errlen, "invalid whole_disk_device");
            seen_mask |= (1u << 8);
            snprintf(out->whole_disk_device, sizeof(out->whole_disk_device), "%s", v);
        } else if (strcmp(k, "supervisor_sock") == 0) {
            if ((seen_mask & (1u << 9)) || !safe_abs(v)) return fail(err, errlen, "invalid supervisor_sock");
            seen_mask |= (1u << 9);
            snprintf(out->supervisor_sock, sizeof(out->supervisor_sock), "%s", v);
        } else if (strcmp(k, "supervisor_exe") == 0) {
            if ((seen_mask & (1u << 10)) || !safe_abs(v)) return fail(err, errlen, "invalid supervisor_exe");
            seen_mask |= (1u << 10);
            snprintf(out->supervisor_exe, sizeof(out->supervisor_exe), "%s", v);
        } else if (strcmp(k, "gateway_sock") == 0) {
            if ((seen_mask & (1u << 11)) || !safe_abs(v)) return fail(err, errlen, "invalid gateway_sock");
            seen_mask |= (1u << 11);
            snprintf(out->gateway_sock, sizeof(out->gateway_sock), "%s", v);
        } else if (strcmp(k, "gateway_exe") == 0) {
            if ((seen_mask & (1u << 12)) || !safe_abs(v)) return fail(err, errlen, "invalid gateway_exe");
            seen_mask |= (1u << 12);
            snprintf(out->gateway_exe, sizeof(out->gateway_exe), "%s", v);
        } else if (strcmp(k, "net_iface") == 0) {
            if ((seen_mask & (1u << 13)) || !v[0] || strlen(v) >= sizeof(out->net_iface))
                return fail(err, errlen, "invalid net_iface");
            seen_mask |= (1u << 13);
            snprintf(out->net_iface, sizeof(out->net_iface), "%s", v);
        } else if (strcmp(k, "gateway_flow_mode") == 0) {
            if (seen_mask & (1u << 14)) return fail(err, errlen, "duplicate gateway_flow_mode");
            seen_mask |= (1u << 14);
            if (strcmp(v, "strict_forward") == 0) out->gateway_flow_mode = R3C_GW_STRICT_FORWARD;
            else if (strcmp(v, "controlled_local") == 0) out->gateway_flow_mode = R3C_GW_CONTROLLED_LOOPBACK;
            else return fail(err, errlen, "unsupported gateway_flow_mode");
        } else if (strcmp(k, "allow_external_degraded") == 0) {
            if (seen_mask & (1u << 15)) return fail(err, errlen, "duplicate allow_external_degraded");
            seen_mask |= (1u << 15);
            if (strcmp(v, "0") == 0) out->allow_external_degraded = 0;
            else if (strcmp(v, "1") == 0) out->allow_external_degraded = 1;
            else return fail(err, errlen, "invalid allow_external_degraded");
        } else if (strcmp(k, "require_sensor_heartbeat") == 0) {
            if (seen_mask & (1u << 16)) return fail(err, errlen, "duplicate require_sensor_heartbeat");
            seen_mask |= (1u << 16);
            if (strcmp(v, "0") == 0) out->require_sensor_heartbeat = 0;
            else if (strcmp(v, "1") == 0) out->require_sensor_heartbeat = 1;
            else return fail(err, errlen, "invalid require_sensor_heartbeat");
        } else if (strcmp(k, "stable_ms") == 0) {
            if ((seen_mask & (1u << 17)) || parse_u64(v, &out->stable_ms) != 0 || out->stable_ms == 0)
                return fail(err, errlen, "invalid stable_ms");
            seen_mask |= (1u << 17);
        } else if (strcmp(k, "max_gap_ms") == 0) {
            if ((seen_mask & (1u << 18)) || parse_u64(v, &out->max_gap_ms) != 0 || out->max_gap_ms == 0)
                return fail(err, errlen, "invalid max_gap_ms");
            seen_mask |= (1u << 18);
        } else if (strcmp(k, "deadline_ms") == 0) {
            if ((seen_mask & (1u << 19)) || parse_u64(v, &out->deadline_ms) != 0 || out->deadline_ms == 0)
                return fail(err, errlen, "invalid deadline_ms");
            seen_mask |= (1u << 19);
        } else if (strcmp(k, "max_evidence_age_ms") == 0) {
            if ((seen_mask & (1u << 20)) || parse_u64(v, &out->max_evidence_age_ms) != 0 || out->max_evidence_age_ms == 0)
                return fail(err, errlen, "invalid max_evidence_age_ms");
            seen_mask |= (1u << 20);
        } else {
            return fail(err, errlen, "unknown policy key");
        }
    }
    if (out->deadline_ms < out->stable_ms)
        return fail(err, errlen, "policy deadline_ms < stable_ms");
    return 0;
}
