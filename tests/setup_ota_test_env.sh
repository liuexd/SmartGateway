#!/bin/sh
#
# D23 OTA 测试环境构建脚本
#
# 背景：D23 的集成测试依赖 build/ota-test/ 下的一批辅助产物。
# 这些产物此前是"手工构建"的，从未提交到仓库，一旦 make clean
# 或换机器就全部丢失，导致 D23 回归测试无法执行。
#
# 本脚本把这些产物固化成可复现的构建流程，解决 S3 的准入条件 A4。
#
# 产出：
#   build/ota-test/ota_baseline_init          还原 CONFIRMED 初始状态
#   build/ota-test/real-x86/v0.1.0-test1/     旧版本发布目录
#   build/ota-test/real-x86/v0.1.1-test1/     新版本发布目录（候选包）
#   build/ota-test/install-x86-fault.2e0Csr/  故障注入用安装目录
#
# 用法：
#   sh tests/setup_ota_test_env.sh
#
set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"

OTA_DIR="$ROOT/bsp/apps/ota_agent"
OUT="$ROOT/build/ota-test"

OLD_VERSION="0.1.0-test1"
NEW_VERSION="0.1.1-test1"
INSTALL="$OUT/install-x86-fault.2e0Csr"

say() { printf '%s\n' "$*"; }
die() { printf 'FAIL: %s\n' "$*" >&2; exit 1; }

say "== D23 OTA 测试环境构建 =="
say "仓库根目录: $ROOT"

# ---------------------------------------------------------------
# 0. 前置：x86 ota_agent 与 Gateway 必须已构建
# ---------------------------------------------------------------
say ""
say "-- 检查前置构建产物 --"

if [ ! -x "$OTA_DIR/ota_agent_x86" ]; then
    say "  构建 ota_agent_x86 ..."
    (cd "$OTA_DIR" && make ota_agent_x86) \
        || die "ota_agent_x86 构建失败"
fi

if [ ! -x "$ROOT/build/smart_gateway" ]; then
    say "  构建 smart_gateway ..."
    make gateway || die "smart_gateway 构建失败"
fi

say "  ota_agent_x86   : OK"
say "  smart_gateway   : OK"

mkdir -p "$OUT"

# ---------------------------------------------------------------
# 1. ota_baseline_init
#
# 测试脚本的调用方式是：
#   ota_baseline_init <install>/state
# 只传**一个**参数（state 目录），且期望它把状态重置为
#   state=CONFIRMED, confirmed=OLD, candidate=OLD
#
# 注意 ota_state_store 的接口是 4 个参数，不是同一个工具。
# 这里写一个薄封装，把单参数形式转换成对 ota_state_store 的调用，
# 避免在测试脚本里散落硬编码。
# ---------------------------------------------------------------
say ""
say "-- 构建 ota_baseline_init --"

cat > "$OUT/ota_baseline_init.c" <<'INIT_C'
/*
 * ota_baseline_init - 把 OTA 安装目录的状态重置为"已确认旧版"基线
 *
 * 用法: ota_baseline_init <state_dir>
 *
 * 这是 D23 集成测试的辅助工具。它只做一件事：
 * 在 state 目录下写出一份 CONFIRMED 状态的 ota_state.json，
 * 其中 confirmed_version 与 candidate_version 都指向旧版本。
 *
 * 之所以单独写而不是直接调用 ota_state_store：
 *   ota_state_store 需要 4 个参数，而测试脚本只传 state 目录。
 *   把"基线是什么"的知识集中在这里，测试脚本不必知道细节。
 *
 * 实现上复用与 ota_state_store 相同的原子写流程：
 *   临时文件 -> fsync -> rename -> fsync(目录)
 * 保证即使中途失败也不会留下半截状态文件误导后续测试。
 */

#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* 与 D23 测试脚本保持一致的版本号 */
#define BASELINE_VERSION "0.1.0-test1"

static int write_all(int fd, const char *data, size_t len)
{
    size_t total = 0;

    while (total < len) {
        ssize_t n = write(fd, data + total, len - total);

        if (n < 0 && errno == EINTR)
            continue;

        if (n <= 0)
            return -1;

        total += (size_t)n;
    }

    return 0;
}

int main(int argc, char **argv)
{
    char temp_path[PATH_MAX];
    char state_path[PATH_MAX];
    char payload[512];

    int fd = -1;
    int dirfd = -1;
    int result = 1;

    if (argc != 2) {
        fprintf(stderr, "Usage: %s <state_dir>\n", argv[0]);
        return 1;
    }

    const char *dir = argv[1];

    int a = snprintf(temp_path, sizeof(temp_path),
                     "%s/ota_state.json.tmp.XXXXXX", dir);
    int b = snprintf(state_path, sizeof(state_path),
                     "%s/ota_state.json", dir);

    if (a < 0 || (size_t)a >= sizeof(temp_path) ||
        b < 0 || (size_t)b >= sizeof(state_path)) {
        fprintf(stderr, "FAIL: path too long\n");
        return 1;
    }

    /* 构造与 ota_state_io.c 期望的 schema 一致的 JSON */
    int p = snprintf(
        payload, sizeof(payload),
        "{\n"
        "  \"schema_version\": 1,\n"
        "  \"state\": \"CONFIRMED\",\n"
        "  \"confirmed_version\": \"%s\",\n"
        "  \"candidate_version\": \"%s\"\n"
        "}\n",
        BASELINE_VERSION, BASELINE_VERSION);

    if (p < 0 || (size_t)p >= sizeof(payload)) {
        fprintf(stderr, "FAIL: payload too long\n");
        return 1;
    }

    fd = mkstemp(temp_path);

    if (fd < 0) {
        perror("mkstemp");
        return 1;
    }

    if (write_all(fd, payload, (size_t)p) != 0) {
        perror("write");
        goto cleanup;
    }

    /* 数据落盘后再改名，避免掉电留下半截文件 */
    if (fsync(fd) != 0) {
        perror("fsync");
        goto cleanup;
    }

    if (close(fd) != 0) {
        fd = -1;
        perror("close");
        goto cleanup;
    }

    fd = -1;

    if (rename(temp_path, state_path) != 0) {
        perror("rename");
        goto cleanup;
    }

    /* 目录项也要落盘 */
    dirfd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);

    if (dirfd >= 0) {
        (void)fsync(dirfd);
        (void)close(dirfd);
        dirfd = -1;
    }

    printf("PASS: baseline state written (%s)\n", BASELINE_VERSION);
    result = 0;

cleanup:
    if (fd >= 0)
        (void)close(fd);

    if (result != 0)
        (void)unlink(temp_path);

    return result;
}
INIT_C

gcc -std=c11 -O1 -g -Wall -Wextra -Werror \
    -o "$OUT/ota_baseline_init" "$OUT/ota_baseline_init.c" \
    || die "ota_baseline_init 构建失败"

say "  $OUT/ota_baseline_init"

# ---------------------------------------------------------------
# 2. 两个发布目录（旧版 / 新版）
#
# 每个目录需要：
#   smart_gateway  x86-64 ELF
#   manifest.json  application/version/architecture/binary/size/sha256
# ---------------------------------------------------------------
say ""
say "-- 构建发布目录 --"

build_release() {
    ver="$1"
    dir="$OUT/real-x86/v$ver"

    rm -rf "$dir"
    mkdir -p "$dir"

    cp "$ROOT/build/smart_gateway" "$dir/smart_gateway"
    chmod +x "$dir/smart_gateway"

    size=$(stat -c%s "$dir/smart_gateway")
    sha=$(sha256sum "$dir/smart_gateway" | cut -d' ' -f1)

    cat > "$dir/manifest.json" <<EOF
{
  "format_version": 1,
  "application": "smart_gateway",
  "version": "$ver",
  "architecture": "x86_64-linux-gnu",
  "binary": "smart_gateway",
  "size": $size,
  "sha256": "$sha"
}
EOF

    say "  $dir  (size=$size)"
}

build_release "$OLD_VERSION"
build_release "$NEW_VERSION"

# ---------------------------------------------------------------
# 3. 安装目录
#
# 结构：
#   <install>/state/ota_state.json      CONFIRMED / 旧版
#   <install>/releases/v0.1.0-test1/    旧版
#   <install>/releases/v0.1.1-test1/    新版
#   <install>/current -> releases/v0.1.0-test1
# ---------------------------------------------------------------
say ""
say "-- 构建安装目录 --"

rm -rf "$INSTALL"
mkdir -p "$INSTALL/state" "$INSTALL/releases"

cp -r "$OUT/real-x86/v$OLD_VERSION" "$INSTALL/releases/v$OLD_VERSION"
cp -r "$OUT/real-x86/v$NEW_VERSION" "$INSTALL/releases/v$NEW_VERSION"

# current 指向旧版；用相对路径，便于整体搬迁
( cd "$INSTALL" && ln -sfn "releases/v$OLD_VERSION" current )

"$OUT/ota_baseline_init" "$INSTALL/state" \
    || die "基线状态初始化失败"

say "  $INSTALL"
say "    state/       CONFIRMED / $OLD_VERSION"
say "    releases/    v$OLD_VERSION, v$NEW_VERSION"
say "    current  ->  releases/v$OLD_VERSION"

# ---------------------------------------------------------------
# 4. 自检
# ---------------------------------------------------------------
say ""
say "-- 自检 --"

fail=0

check() {
    if [ -e "$1" ]; then
        say "  OK   $2"
    else
        say "  MISS $2"
        fail=1
    fi
}

check "$OUT/ota_baseline_init"                  "ota_baseline_init"
check "$OUT/real-x86/v$OLD_VERSION/smart_gateway" "旧版二进制"
check "$OUT/real-x86/v$OLD_VERSION/manifest.json" "旧版 manifest"
check "$OUT/real-x86/v$NEW_VERSION/smart_gateway" "新版二进制"
check "$OUT/real-x86/v$NEW_VERSION/manifest.json" "新版 manifest"
check "$INSTALL/state/ota_state.json"           "初始状态文件"
check "$INSTALL/current"                        "current 软链"

if [ "$fail" -ne 0 ]; then
    die "产物不完整"
fi

say ""
say "环境就绪。运行 D23 回归："
say "  python3 tests/test_ota_northbound_fault.py --prepare"
