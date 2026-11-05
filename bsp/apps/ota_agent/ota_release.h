#ifndef OTA_RELEASE_H
#define OTA_RELEASE_H

/*
 * 将经过验证的升级包安装到 releases/v版本号。
 *
 * 只安装候选文件，不切换 current，
 * 也不改变 OTA 状态文件。
 *
 * 调用方必须持有 OTA 排他锁。
 */
int ota_release_stage(
    const char *package_dir,
    const char *install_dir);

#endif
