#include "health_gateway.h"
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>

int main(int argc, char **argv)
{
    if (argc != 6) return 2;
    if (sgw_health_gateway_probe(argv[1], (uid_t)strtoul(argv[2], NULL, 10),
                                 (pid_t)strtol(argv[3], NULL, 10),
                                 strtoull(argv[4], NULL, 10), atoi(argv[5])) != 0) {
        puts("unverified");
        return 1;
    }
    puts("components-ready");
    return 0;
}
