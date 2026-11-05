#define _POSIX_C_SOURCE 200809L

#include "ota_lock.h"

#include <errno.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

int ota_lock_acquire(
    const char *install_dir,
    int exclusive
)
{
    int dirfd;
    int lockfd;
    int saved_errno;
    struct stat st;

    if (install_dir == NULL || install_dir[0] == '\0') {
        errno = EINVAL;
        return -1;
    }

    /* 打开可信OTA安装目录 */
    dirfd = open(
        install_dir, O_RDONLY | O_DIRECTORY |
        O_CLOEXEC | O_NOFOLLOW);

    if(dirfd < 0)
        return -1;
    
    /*
     * 锁文件固定放在安装根目录。
     * 不跟随符号链接。
     */
    lockfd = openat(
        dirfd,
        ".ota.lock",
        O_RDWR | O_CREAT |
        O_CLOEXEC | O_NOFOLLOW,
        0600        
    );

    saved_errno = errno;
    close(dirfd);
    
    if(lockfd < 0)
    {
        errno = saved_errno;
        return -1;
    }

    /* 锁对象必须是普通文件 */
    if(fstat(lockfd, &st) !=0 ||
        !S_ISREG(st.st_mode))
    {
        saved_errno = errno;
        if(saved_errno == 0)
            saved_errno = EINVAL;

        close(lockfd);

        errno =saved_errno;
        return -1;
    }

    int operation = exclusive ? LOCK_EX : LOCK_SH;

    /*
     * LOCK_NB：不等待。
     * 如果已有冲突锁，立即返回 EWOULDBLOCK。
     */
    if(flock(lockfd, operation | LOCK_NB) != 0){
        saved_errno = errno;
        close(lockfd);
        errno = saved_errno; 
        return -1;
    }

    return lockfd;
}

void ota_lock_release(int lock_fd)
{
    if(lock_fd >= 0)
    {
        flock(lock_fd, LOCK_UN);
        close(lock_fd);
    }
}