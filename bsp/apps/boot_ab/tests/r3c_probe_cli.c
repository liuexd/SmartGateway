#define _GNU_SOURCE
#include "r3c_data.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    char reason[256] = {0};
    int result = -1;
    if (argc == 3 && strcmp(argv[1], "app") == 0)
        result = r3c_app_ota_inspect(argv[2], getuid(), reason, sizeof(reason));
    else if (argc == 3 && strcmp(argv[1], "hold-app") == 0) {
        int held = -1;
        result = r3c_app_ota_inspect_locked(argv[2], getuid(), &held,
                                            reason, sizeof(reason));
        if (result == 0) {
            puts("LOCKED");
            fflush(stdout);
            (void)getchar();
            (void)flock(held, LOCK_UN);
            close(held);
        }
    }
    else if (argc == 4 && strcmp(argv[1], "data") == 0) {
        r3c_data_policy_t policy = {argv[2], argv[3], 1, getuid()};
        result = r3c_data_inspect(&policy, reason, sizeof(reason));
    } else if ((argc == 3 || argc == 4) && strcmp(argv[1], "contents") == 0) {
        int fd = open(argv[2], O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        unsigned long long minimum = argc == 4 ? strtoull(argv[3], NULL, 10) : 1024 * 1024;
        result = r3c_data_inspect_contents(fd, getuid(), minimum, reason, sizeof(reason));
        if (fd >= 0) close(fd);
    } else if (argc == 3 && strcmp(argv[1], "mount") == 0) {
        char *text = NULL;
        size_t cap = 0, used = 0;
        char *line = NULL;
        size_t n = 0;
        ssize_t len;
        while ((len = getline(&line, &n, stdin)) > 0) {
            char *next = realloc(text, used + (size_t)len + 1);
            if (!next) { free(text); free(line); return 2; }
            text = next; memcpy(text + used, line, (size_t)len);
            used += (size_t)len; text[used] = '\0';
        }
        free(line);
        (void)cap;
        result = r3c_data_mountinfo_matches(text, argv[2], 179, 3);
        free(text);
    } else return 2;
    printf("%s\n", reason);
    return result == 0 ? 0 : 1;
}
