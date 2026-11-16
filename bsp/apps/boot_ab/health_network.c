#define _GNU_SOURCE
#include "health_network.h"
#include "health_supervisor.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define POLICY "/etc/smartgateway/system-health-network.conf"

sgw_health_state_t sgw_health_network_probe(const char *interface_name)
{
    struct ifaddrs *addresses = NULL, *p;
    int seen = 0, up = 0, address = 0;
    size_t len, i;

    if (!interface_name) return SGW_HEALTH_UNKNOWN;
    len = strlen(interface_name);
    if (len == 0 || len >= IF_NAMESIZE) return SGW_HEALTH_UNKNOWN;
    for (i = 0; i < len; ++i) {
        unsigned char c = (unsigned char)interface_name[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.'))
            return SGW_HEALTH_UNKNOWN;
    }
    if (getifaddrs(&addresses) != 0) return SGW_HEALTH_UNKNOWN;
    for (p = addresses; p; p = p->ifa_next) {
        if (!p->ifa_name || strcmp(p->ifa_name, interface_name) != 0) continue;
        seen = 1;
        if ((p->ifa_flags & IFF_LOOPBACK) || !(p->ifa_flags & IFF_UP) ||
            !(p->ifa_flags & IFF_RUNNING)) continue;
        up = 1;
        if (!p->ifa_addr) continue;
        if (p->ifa_addr->sa_family == AF_INET) {
            const struct sockaddr_in *a = (const struct sockaddr_in *)p->ifa_addr;
            unsigned long ip = ntohl(a->sin_addr.s_addr);
            if (ip != 0 && (ip >> 24) != 127 && (ip >> 28) < 14)
                address = 1;
        } else if (p->ifa_addr->sa_family == AF_INET6) {
            const struct sockaddr_in6 *a = (const struct sockaddr_in6 *)p->ifa_addr;
            if (!IN6_IS_ADDR_UNSPECIFIED(&a->sin6_addr) &&
                !IN6_IS_ADDR_LOOPBACK(&a->sin6_addr) &&
                !IN6_IS_ADDR_MULTICAST(&a->sin6_addr))
                address = 1;
        }
    }
    freeifaddrs(addresses);
    return seen && up && address ? SGW_HEALTH_HEALTHY : SGW_HEALTH_UNHEALTHY;
}

sgw_health_state_t sgw_health_network_policy(void)
{
    char buf[128], interface_name[IF_NAMESIZE];
    struct stat st;
    ssize_t n;
    int fd;
    const char *prefix = "interface=";
    size_t len;

    /* Policy must be explicitly installed by a trusted administrator. */
    if (sgw_health_trusted_path(POLICY, 0, 0) != 0)
        return SGW_HEALTH_UNKNOWN;
    fd = open(POLICY, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return SGW_HEALTH_UNKNOWN;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_uid != 0 ||
        (st.st_mode & (S_IWGRP | S_IWOTH)) || st.st_size <= 0 ||
        st.st_size >= (off_t)sizeof(buf)) {
        close(fd);
        return SGW_HEALTH_UNKNOWN;
    }
    n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0 || n != st.st_size) return SGW_HEALTH_UNKNOWN;
    buf[n] = '\0';
    len = strlen(prefix);
    if (strncmp(buf, prefix, len) != 0 || buf[n - 1] != '\n' ||
        (size_t)n <= len + 1 || (size_t)n - len - 1 >= sizeof(interface_name))
        return SGW_HEALTH_UNKNOWN;
    memcpy(interface_name, buf + len, (size_t)n - len - 1);
    interface_name[(size_t)n - len - 1] = '\0';
    return sgw_health_network_probe(interface_name);
}
