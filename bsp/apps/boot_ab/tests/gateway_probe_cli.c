#include "health_gateway.h"
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>

int main(int argc, char **argv)
{
    int flow = argc == 7 && argv[6][0] == 'f' && argv[6][1] == '\0';
    int rc;
    if (argc != 6 && !flow) return 2;
    rc = flow ? sgw_health_gateway_flow_probe(argv[1], (uid_t)strtoul(argv[2], NULL, 10),
                                              (pid_t)strtol(argv[3], NULL, 10),
                                              strtoull(argv[4], NULL, 10), atoi(argv[5])) :
                sgw_health_gateway_probe(argv[1], (uid_t)strtoul(argv[2], NULL, 10),
                                         (pid_t)strtol(argv[3], NULL, 10),
                                         strtoull(argv[4], NULL, 10), atoi(argv[5]));
    if (rc != 0) {
        puts("unverified");
        return 1;
    }
    puts(flow ? "flow-observed" : "components-ready");
    return 0;
}
