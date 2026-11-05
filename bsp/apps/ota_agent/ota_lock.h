#ifndef OTA_LOCK_H
#define OTA_LOCK_H

/*
 * exclusive = 1：排他锁，供升级、回滚使用。
 * exclusive = 0：共享锁，供状态查询使用。
 *
 * 成功：返回锁文件描述符。
 * 失败：返回 -1，并设置 errno。
 */
int ota_lock_acquire(
    const char *install_dir,
    int exclusive);

void ota_lock_release(int lock_fd);

#endif