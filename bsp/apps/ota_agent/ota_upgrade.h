#ifndef OTA_UPGRADE_H
#define OTA_UPGRADE_H

/*
 * 实验性版本激活：
 * 只更新 current 和 OTA 状态，不管理 Gateway 进程。
 *
 * 调用方必须持有 OTA 排他锁。
 * 成功返回 0，失败返回 -1。
 */
int ota_activate_test(
    const char *package_dir,
    const char *install_dir);

#endif