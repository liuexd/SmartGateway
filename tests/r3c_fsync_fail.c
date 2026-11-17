#include <errno.h>

/* Loaded only into the isolated test CLI; never part of a production build. */
int fsync(int fd)
{
    (void)fd;
    errno = EIO;
    return -1;
}
