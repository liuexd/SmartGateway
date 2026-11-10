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

/*
 * 激活前的**只读**校验（S3-R2）。
 *
 * 执行与 ota_activate_test() 完全相同的一组校验，但：
 *   - 不写状态文件
 *   - 不切换 current
 *   - 不创建/删除任何文件
 *
 * 用途：调用方在产生副作用（尤其是停止正在运行的 Gateway）
 * 之前，先确认"这次升级能否成功"。若返回 -1，应直接放弃升级，
 * 不要触碰正在运行的业务。
 *
 * 校验内容：
 *   1. 无进行中的 OTA 事务（状态为 CONFIRMED 或 ROLLED_BACK）
 *   2. current 指向已确认版本
 *   3. 升级包合法（manifest 字段、大小、SHA-256、ELF 架构）
 *   4. 候选版本 ≠ 已确认版本
 *   5. 已确认版本文件完整且版本号自洽
 *   6. 候选版本已安装，且 SHA-256 与升级包一致
 *
 * 实现上与 ota_activate_test() 共用同一份校验函数，
 * 因此不会出现"预检放行但激活被拒"的规则分叉。
 *
 * 返回 0 表示可以安全进入变更阶段；-1 表示不可升级。
 */
int ota_activate_precheck(
    const char *package_dir,
    const char *install_dir);

#endif