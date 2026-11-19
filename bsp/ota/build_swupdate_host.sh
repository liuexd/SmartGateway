#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
BR_DL="${BR_DL_DIR:-$HOME/Desktop/Buildroot_2020.02.x/dl}"
SWU_TARBALL="${SWU_TARBALL:-$BR_DL/swupdate/swupdate-2019.11.tar.gz}"
LIBCFG_TARBALL="${LIBCFG_TARBALL:-$BR_DL/libconfig/libconfig-1.7.2.tar.gz}"
OUT_DIR="${SWUPDATE_HOST_OUT_DIR:-$REPO_DIR/build/swupdate-host}"
DEPS_DIR="$OUT_DIR/deps"
SRC_PARENT="$OUT_DIR/work"
JOBS="${JOBS:-$(nproc)}"

if [ ! -f "$SWU_TARBALL" ]; then
    echo "ERROR: SWUpdate tarball not found: $SWU_TARBALL" >&2
    exit 1
fi
if [ ! -f "$LIBCFG_TARBALL" ]; then
    echo "ERROR: libconfig tarball not found: $LIBCFG_TARBALL" >&2
    exit 1
fi
if [ ! -f "$SCRIPT_DIR/swupdate.config" ]; then
    echo "ERROR: swupdate.config not found in $SCRIPT_DIR" >&2
    exit 1
fi

rm -rf "$SRC_PARENT" "$DEPS_DIR"
mkdir -p "$OUT_DIR" "$DEPS_DIR/include" "$DEPS_DIR/lib" "$SRC_PARENT"

echo "=== Building static libconfig for host SWUpdate ==="
tar -xzf "$LIBCFG_TARBALL" -C "$SRC_PARENT"
LIBCFG_SRC="$SRC_PARENT/libconfig-1.7.2"
LIBCFG_CFLAGS=(
    -O2 -std=gnu11 -fPIC -Wno-implicit-function-declaration -fcommon
    -D_GNU_SOURCE -D_REENTRANT -DPARSER_PREFIX='"libconfig_yy"'
    -DHAVE_FREELOCALE=1 -DHAVE_NEWLOCALE=1 -DHAVE_USELOCALE=1
    -DHAVE_INTTYPES_H=1 -DHAVE_STDINT_H=1 -DHAVE_STDLIB_H=1
    -DHAVE_STRINGS_H=1 -DHAVE_STRING_H=1 -DHAVE_SYS_STAT_H=1
    -DHAVE_SYS_TYPES_H=1 -DHAVE_UNISTD_H=1
    -I"$LIBCFG_SRC/lib"
)
(
    cd "$LIBCFG_SRC/lib"
    for src in grammar.c libconfig.c scanctx.c scanner.c strbuf.c strvec.c util.c wincompat.c; do
        gcc "${LIBCFG_CFLAGS[@]}" -c "$src" -o "${src%.c}.o"
    done
    ar rcs "$DEPS_DIR/lib/libconfig.a" grammar.o libconfig.o scanctx.o scanner.o strbuf.o strvec.o util.o wincompat.o
    cp -f libconfig.h "$DEPS_DIR/include/libconfig.h"
)

echo "=== Extracting and building host swupdate 2019.11 ==="
tar -xzf "$SWU_TARBALL" -C "$SRC_PARENT"
SWU_SRC="$SRC_PARENT/swupdate-2019.11"
cp -f "$SCRIPT_DIR/swupdate.config" "$SWU_SRC/.config"

HAVE_FLAGS=(
    HAVE_LINUX=y
    HAVE_FREEBSD=n
    HAVE_LIBSSL=y
    HAVE_LIBCRYPTO=y
    HAVE_LIBCONFIG=y
    HAVE_JSON_C=y
    HAVE_ZLIB=n
    HAVE_LIBUBOOTENV=n
    HAVE_LIBCURL=n
    HAVE_LUA=n
    HAVE_LIBARCHIVE=n
    HAVE_LIBMTD=n
    HAVE_LIBUBI=n
    HAVE_LIBZEROMQ=n
    HAVE_LIBGPIOD=n
    HAVE_LIBEBGENV=n
    HAVE_URIPARSER=n
    HAVE_LIBWEBSOCKETS=n
    HAVE_ZSTD=n
)

HOST_CC_COMPAT="gcc -std=gnu11 -Wno-implicit-function-declaration -fcommon"

make -C "$SWU_SRC" "${HAVE_FLAGS[@]}" \
    HOSTCC="$HOST_CC_COMPAT" CC="gcc" LD="gcc" olddefconfig

make -C "$SWU_SRC" -j"$JOBS" "${HAVE_FLAGS[@]}" \
    HOSTCC="$HOST_CC_COMPAT" \
    CC="gcc" \
    LD="gcc" \
    CONFIG_EXTRA_CFLAGS="-I$DEPS_DIR/include -Wno-deprecated-declarations" \
    CONFIG_EXTRA_LDFLAGS="-L$DEPS_DIR/lib"

install -m 0755 "$SWU_SRC/swupdate" "$OUT_DIR/swupdate"

echo "=== Verifying built host swupdate signature enforcement ==="
"$OUT_DIR/swupdate" --version
if ! strings "$OUT_DIR/swupdate" | grep -Fq "swupdate built for signed image, provide a public key file"; then
    echo "ERROR: built swupdate binary missing mandatory signed-image key check" >&2
    exit 1
fi
if strings "$OUT_DIR/swupdate" | grep -Fq "public key / cert %s ignored"; then
    echo "ERROR: built swupdate binary has ignored-key fallback (SIGALG misconfigured)" >&2
    exit 1
fi
echo "OK: $OUT_DIR/swupdate built with CONFIG_SIGNED_IMAGES + CONFIG_SIGALG_RAWRSA"
