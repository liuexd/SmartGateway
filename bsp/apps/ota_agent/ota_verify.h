#ifndef OTA_VERIFY_H
#define OTA_VERIFY_H

/*
 * 校验指定目录下的 OTA 升级包。
 *
 * 返回值：
 *   0  校验成功
 *  非0 校验失败
 *
 * 本函数只校验，不安装、不切换版本。
 */
int ota_verify_package(const char *package_directory);

#endif
