
#include "ota_state_io.h"

#include <stdio.h>
#include <string.h>

#define CHECK(expr) do {                         \
    if (!(expr)) {                               \
        fprintf(stderr, "FAIL line %d\n",        \
                __LINE__);                      \
        return 1;                                \
    }                                            \
} while (0)

int main(int argc, char **argv)
{
    ota_state_record_t saved = {
        .state = OTA_STAGED,
        .confirmed_version = "0.1.0-test1",
        .candidate_version = "0.1.1-test1"
    };

    ota_state_record_t loaded = {0};

    if (argc != 2) {
        fprintf(stderr, "Usage: %s DIR\n", argv[0]);
        return 1;
    }

    puts("=== Test 1: save STAGED ===");

    CHECK(ota_state_save(argv[1], &saved) == 0);
    CHECK(ota_state_load(argv[1], &loaded) == 0);

    CHECK(loaded.state == OTA_STAGED);
    CHECK(strcmp(
        loaded.confirmed_version,
        "0.1.0-test1") == 0);

    CHECK(strcmp(
        loaded.candidate_version,
        "0.1.1-test1") == 0);

    puts("=== Test 2: update ACTIVATED ===");

    saved.state = OTA_ACTIVATED;

    CHECK(ota_state_save(argv[1], &saved) == 0);
    CHECK(ota_state_load(argv[1], &loaded) == 0);

    CHECK(loaded.state == OTA_ACTIVATED);

    /* 新版尚未确认，旧的 confirmed 必须保留 */
    CHECK(strcmp(
        loaded.confirmed_version,
        "0.1.0-test1") == 0);
    puts("=== Test 3: begin new upgrade ===");

    /* 模拟当前已经确认旧版本 */
    saved.state = OTA_CONFIRMED;

    strcpy(saved.candidate_version, "0.1.0-test1");

    CHECK(ota_state_save(argv[1], &saved) == 0);
    CHECK(ota_state_load(argv[1], &loaded) == 0);

    /* 正式开始下一轮升级 */
    CHECK(ota_state_begin_upgrade(
        argv[1], &loaded, "0.1.1-test1") == 0);

    /* 从磁盘重新读取，验证持久化结果 */
    CHECK(ota_state_load(argv[1], &loaded) == 0);

    CHECK(loaded.state == OTA_DOWNLOADED);

    CHECK(strcmp(
        loaded.confirmed_version,
        "0.1.0-test1") == 0);

    CHECK(strcmp(
        loaded.candidate_version,
        "0.1.1-test1") == 0);

    /* 同一事务不能重复开始 */
    CHECK(ota_state_begin_upgrade(
        argv[1], &loaded, "0.1.2-test1") == -1);
  
    puts("PASS: state IO module tests");
    return 0;
}
