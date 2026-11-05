
#ifndef OTA_STATE_IO_H
#define OTA_STATE_IO_H

#include "ota_state.h"

#define OTA_VERSION_MAX 64

typedef struct {
    ota_state_t state;

    char confirmed_version[OTA_VERSION_MAX];
    char candidate_version[OTA_VERSION_MAX];
} ota_state_record_t;

/* 保存完整 OTA 状态，成功返回 0 */
int ota_state_save(
    const char *directory,
    const ota_state_record_t *record);

/* 读取并验证 OTA 状态，成功返回 0 */
int ota_state_load(
    const char *directory,
    ota_state_record_t *record);

/*
 * 开始新升级事务。
 *
 * 前提：
 * 1. 调用方已验证升级包及其版本。
 * 2. 调用方持有 OTA 排他锁。
 * 3. 当前已确认版本与 current 一致。
 *
 * 保留 confirmed_version，
 * 将 candidate_version 更新为新版本，
 * 并持久化 DOWNLOADED 状态。
 */
int ota_state_begin_upgrade(
    const char *state_directory,
    ota_state_record_t *record,
    const char *candidate_version);

#endif
