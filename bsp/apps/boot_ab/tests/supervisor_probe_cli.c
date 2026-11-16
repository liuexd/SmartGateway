#include "health_supervisor.h"
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
    pid_t pid = 0;
    int result;
    if (argc != 6) return 2;
    result = sgw_health_supervisor_status(argv[1], argv[2], argv[3],
                                          (uid_t)strtoul(argv[4], NULL, 10),
                                          atoi(argv[5]), &pid);
    if (result == 0) {
        printf("managed_pid=%ld\n", (long)pid);
        return 0;
    }
    puts("unverified");
    return 1;
}
