#!/bin/sh
#
# D25-1 ARM 产物与目标环境核验脚本
#
# 用途：
#   1. 在宿主机上静态检查 ARM ELF（架构、解释器、依赖、ABI、
#      所需 glibc 符号版本）。
#   2. 在目标板上运行时环境核验（CPU、ABI、内核、C 库、
#      链接器、依赖库是否存在、存储目录可写性）。
#
# 设计原则：
#   - 默认全部只读，不修改系统；
#   - 不做任何 OTA 激活/切换；
#   - 任何"未执行"的检查都必须显式输出 SKIP 或 BLOCKED，
#     不允许把未运行的项目写成 PASS。
#
# 用法：
#   ./bsp/tools/d25-1-arm-check.sh host   [ARM_BIN...]
#   ./bsp/tools/d25-1-arm-check.sh target
#
set -u

MODE="${1:-host}"
shift 2>/dev/null || true

# 与 Makefile 保持一致的默认值
SDK_DIR="${ARM_SDK_DIR:-$HOME/Desktop/ToolChain/arm-buildroot-linux-gnueabihf_sdk-buildroot}"
CROSS="${ARM_CROSS:-$SDK_DIR/bin/arm-buildroot-linux-gnueabihf-}"
SYSROOT="${ARM_SYSROOT:-$SDK_DIR/arm-buildroot-linux-gnueabihf/sysroot}"

fail=0

say()  { printf '%s\n' "$*"; }
head1() { printf '\n== %s ==\n' "$*"; }
ok()   { printf '  PASS    %s\n' "$*"; }
bad()  { printf '  FAIL    %s\n' "$*"; fail=1; }
skip() { printf '  SKIP    %s\n' "$*"; }
note() { printf '  NOTE    %s\n' "$*"; }

# ------------------------------------------------------------
# 宿主机静态检查
# ------------------------------------------------------------
check_host() {
    head1 "交叉工具链"
    if [ -x "${CROSS}gcc" ]; then
        ok "cross gcc: $("${CROSS}gcc" --version | head -1)"
    else
        bad "cross gcc 不存在: ${CROSS}gcc"
        return 1
    fi

    if [ -x "${CROSS}readelf" ]; then
        ok "cross readelf 可用"
    else
        bad "cross readelf 不存在"
    fi

    if [ -d "$SYSROOT" ]; then
        ok "target sysroot: $SYSROOT"
    else
        bad "target sysroot 不存在: $SYSROOT"
        return 1
    fi

    head1 "目标 sysroot 关键库"
    # glibc 版本决定必须满足的最低版本要求
    LIBC="$SYSROOT/lib/libc.so.6"
    if [ -e "$LIBC" ]; then
        ver=$(strings "$LIBC" 2>/dev/null | grep -oE 'stable release version [0-9.]+' | head -1)
        say "  libc.so.6        : ${ver:-unknown}"
    else
        bad "sysroot 缺少 libc.so.6"
    fi

    LD=$(readlink "$SYSROOT/lib/ld-linux-armhf.so.3" 2>/dev/null)
    if [ -n "$LD" ]; then
        say "  ld-linux-armhf   : -> ${LD}"
    else
        bad "sysroot 缺少 ld-linux-armhf.so.3"
    fi

    for lib in libssl.so.1.1 libjson-c.so.4; do
        if [ -e "$SYSROOT/usr/lib/$lib" ]; then
            say "  $lib : 存在"
        else
            note "$lib 不在 sysroot/usr/lib（可能由目标板提供）"
        fi
    done

    head1 "ARM ELF 检查"
    if [ "$#" -eq 0 ]; then
        set -- build/arm-d25/smart_gateway \
               build/arm-d25/ota_agent \
               build/arm-d25/ota_verify
    fi

    for bin in "$@"; do
        say ""
        say "  --- $bin"
        if [ ! -f "$bin" ]; then
            bad "文件不存在: $bin"
            continue
        fi

        info=$(file -b "$bin")
        case "$info" in
            *"ELF 32-bit"*ARM*) ok "32 位 ARM ELF" ;;
            *) bad "不是 32 位 ARM ELF: $info"; continue ;;
        esac

        case "$info" in
            *"EABI5"*) ok "EABI5" ;;
            *) bad "非 EABI5" ;;
        esac

        interp=$("${CROSS}readelf" -l "$bin" 2>/dev/null \
                 | sed -n 's/.*program interpreter: \(.*\)]/\1/p')
        if [ "$interp" = "/lib/ld-linux-armhf.so.3" ]; then
            ok "解释器 $interp"
        else
            bad "解释器异常: ${interp:-<none>}"
        fi

        # hard-float 判定
        abi=$("${CROSS}readelf" -A "$bin" 2>/dev/null)
        case "$abi" in
            *"Tag_ABI_VFP_args: VFP registers"*) ok "hard-float (VFP registers)" ;;
            *) bad "非 hard-float ABI" ;;
        esac

        # 依赖库
        say "  依赖库:"
        "${CROSS}readelf" -d "$bin" 2>/dev/null \
            | sed -n 's/.*Shared library: \[\(.*\)\]/    \1/p' \
            | while read -r lib; do printf '    %s\n' "$lib"; done

        # 不能引用宿主机独有路径
        if "${CROSS}readelf" -d "$bin" 2>/dev/null | grep -q "RUNPATH\|RPATH"; then
            bad "存在 RPATH/RUNPATH，需确认不会指向宿主机路径"
        else
            ok "无 RPATH/RUNPATH（使用目标板默认 /lib,/usr/lib）"
        fi

        # glibc 符号版本上限
        need=$("${CROSS}readelf" --version-info "$bin" 2>/dev/null \
               | grep -oE 'GLIBC_[0-9]+\.[0-9]+' | sort -u -V | tail -1)
        if [ -n "$need" ]; then
            say "  最高 glibc 符号需求: $need"
        else
            say "  最高 glibc 符号需求: 无（静态或未使用带版本符号）"
        fi
    done
}

# ------------------------------------------------------------
# 目标板运行时检查
# ------------------------------------------------------------
check_target() {
    head1 "CPU 与 ABI"
    say "  uname -m      : $(uname -m)"
    say "  uname -r      : $(uname -r)"
    if grep -q "ARMv7" /proc/cpuinfo 2>/dev/null; then
        ok "CPU 为 ARMv7"
    else
        bad "CPU 不是 ARMv7"
    fi
    # VFP：ARMv7 且含 vfp 表示有浮点单元
    if grep -qi "vfp" /proc/cpuinfo 2>/dev/null; then
        ok "cpuinfo 报告 VFP 浮点单元"
    else
        note "cpuinfo 未显示 VFP，hard-float 需进一步确认"
    fi

    head1 "C 库与动态链接器"
    LIBC=/lib/libc.so.6
    if [ -e "$LIBC" ]; then
        v=$(strings "$LIBC" 2>/dev/null | grep -oE 'stable release version [0-9.]+' | head -1)
        say "  libc          : ${v:-unknown}"
        # 与产物需求对比
        for bin in "$@"; do
            [ -f "$bin" ] || continue
            need=$(readelf --version-info "$bin" 2>/dev/null \
                   | grep -oE 'GLIBC_[0-9]+\.[0-9]+' | sort -u -V | tail -1)
            [ -n "$need" ] && say "  $(basename "$bin") 需: $need"
        done
    else
        bad "缺少 /lib/libc.so.6"
    fi

    ld=$(ls -l /lib/ld-linux-armhf.so.3 2>/dev/null)
    if [ -n "$ld" ]; then
        say "  ld-linux-armhf: $ld"
    else
        bad "缺少 /lib/ld-linux-armhf.so.3"
    fi

    head1 "第三方依赖库"
    for lib in libssl.so.1.1 libcrypto.so.1.1 libjson-c.so.4 libpthread.so.0; do
        found=""
        for d in /lib /usr/lib /lib/arm-linux-gnueabihf /usr/lib/arm-linux-gnueabihf; do
            [ -e "$d/$lib" ] && found="$d/$lib" && break
        done
        if [ -n "$found" ]; then
            ok "$lib -> $found"
        else
            bad "$lib 缺失"
        fi
    done

    head1 "存储与挂载"
    for p in / /tmp; do
        if [ -d "$p" ]; then
            say "  $p"
            df -h "$p" 2>/dev/null | tail -1 | sed 's/^/    /'
            findmnt -no FSTYPE,OPTIONS "$p" 2>/dev/null | sed 's/^/    fstype/'
        fi
    done

    head1 "运行中的正式 Gateway（只读观察）"
    if pgrep -af smart_gateway >/dev/null 2>&1; then
        pgrep -af smart_gateway | sed 's/^/  /'
        ok "检测到运行中的 Gateway（本脚本不会停止它）"
    else
        note "未检测到运行中的 Gateway"
    fi

    head1 "监听端口"
    if command -v ss >/dev/null 2>&1; then
        ss -ltn 2>/dev/null | sed 's/^/  /'
    elif command -v netstat >/dev/null 2>&1; then
        netstat -ltn 2>/dev/null | sed 's/^/  /'
    else
        skip "ss/netstat 均不可用"
    fi

    head1 "OTA 安装目录（只读核查）"
    for d in /opt/sgw /opt/smartgateway /userdata /data; do
        if [ -d "$d" ]; then
            perms=$(ls -ld "$d" | awk '{print $1, $3, $4}')
            say "  $d : $perms"
            note "是否持久化需重启验证，不能仅凭路径判断"
        fi
    done
}

case "$MODE" in
    host)
        check_host "$@"
        ;;
    target)
        check_target "$@"
        ;;
    *)
        say "用法: $0 {host|target} [BIN...]"
        exit 2
        ;;
esac

say ""
if [ "$fail" -ne 0 ]; then
    say "检查项中存在 FAIL，请按上文逐条确认。"
    exit 1
fi
say "所执行的检查项全部通过（未执行的项显示 SKIP/NOTE）。"
exit 0
