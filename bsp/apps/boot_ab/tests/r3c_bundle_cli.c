#define _GNU_SOURCE
#include "r3c_bundle.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    int bundle, tx, key, rc;
    char id[65];
    if (argc != 6) return 2;
    bundle = open(argv[1], O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    tx = open(argv[2], O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    key = open(argv[3], O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    rc = r3c_bundle_record(bundle, tx, key, getuid(),
                            (unsigned int)strtoul(argv[4], NULL, 10),
                            (unsigned int)strtoul(argv[5], NULL, 10), id);
    if (bundle >= 0) close(bundle);
    if (tx >= 0) close(tx);
    if (key >= 0) close(key);
    if (rc != 0) { puts("rejected"); return 1; }
    puts(id);
    return 0;
}
