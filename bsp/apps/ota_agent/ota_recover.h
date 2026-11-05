#ifndef OTA_RECOVER_H
#define OTA_RECOVER_H

/*
 * 恢复 OTA 安装目录到上一个确认版本。
 *
 * 返回：
 * 0：恢复成功或已处于正确状态
 * 1：恢复失败
 */
int ota_recover_execute(const char *install_dir);

#endif