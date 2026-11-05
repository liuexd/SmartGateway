#ifndef OTA_UPGRADE_RUN_H
#define OTA_UPGRADE_RUN_H

/* 调用方必须已经持有安装目录的 OTA 排他锁 */
int ota_upgrade_run_test(
    const char *package_dir,
    const char *install_dir);

#endif
