#!/bin/sh
# Populates .toolchains/device-sysroot/ for UI (LVGL + DRM) cross-compiling.
#
# UI targets link against the K230's *actual* on-device liblvgl.so/libdrm.so
# rather than a self-built LVGL, since the stock image already ships a
# proven, working combination (k230_phone_ui uses it) - see
# devices/k230/README.md. This script pulls those .so files plus matching
# upstream headers, so a caller only needs to re-run it, not follow a
# multi-step manual recipe.
#
# Usage: devices/k230/fetch-device-sysroot.sh [device-ssh-host]
# Default device-ssh-host is root@192.168.8.134 (this project's bench unit).

set -e

DEVICE="${1:-root@192.168.8.134}"
ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
SYSROOT="$ROOT_DIR/.toolchains/device-sysroot"
LVGL_SRC="$ROOT_DIR/.toolchains/lvgl-src"
LIBDRM_SRC="$ROOT_DIR/.toolchains/libdrm-src"

mkdir -p "$SYSROOT/lib" "$SYSROOT/include/drm"

echo "== LVGL headers (v9.5.0, matching the device's liblvgl.so.9.5.0) =="
if [ ! -d "$LVGL_SRC/.git" ]; then
  git clone --depth 1 --branch v9.5.0 https://github.com/lvgl/lvgl.git "$LVGL_SRC"
fi
mkdir -p "$SYSROOT/include/lvgl"
cp -r "$LVGL_SRC"/* "$SYSROOT/include/lvgl/"

echo "== lv_conf.h (kendryte/k230_sdk's own Buildroot package config) =="
gh api "repos/kendryte/k230_sdk/contents/src/little/buildroot-ext/package/lvgl/port_src/lv_conf.h" \
  --jq '.content' | base64 -d > "$SYSROOT/include/lv_conf.h"

echo "== libdrm headers (2.4.124, matching the device's libdrm.so.2.124.0) =="
if [ ! -d "$LIBDRM_SRC/.git" ]; then
  git clone --depth 1 --branch libdrm-2.4.124 https://gitlab.freedesktop.org/mesa/drm.git "$LIBDRM_SRC"
fi
cp "$LIBDRM_SRC/xf86drm.h" "$LIBDRM_SRC/xf86drmMode.h" "$SYSROOT/include/"
cp "$LIBDRM_SRC"/include/drm/*.h "$SYSROOT/include/drm/"

echo "== Shared libraries from the device (liblvgl and its transitive deps) =="
# Pulling the actual .so files, not just headers, is deliberate: it
# guarantees exact ABI/symbol-version compatibility with what will really
# be loaded at runtime, rather than trusting a same-numbered but
# independently-built library to match. liblvgl.so pulls in freetype,
# ffmpeg (av*/sws*), and libevdev at this build's configuration; their own
# further transitive deps (OpenSSL, libpng, liblzma, ...) are deliberately
# NOT pulled here - see --allow-shlib-undefined in
# toolchain-riscv64-linux-gnu.cmake for why that's fine.
scp "$DEVICE:/usr/lib/liblvgl.so*" \
    "$DEVICE:/usr/lib/liblvgl_linux.so" \
    "$DEVICE:/usr/lib/liblvgl_thorvg.so*" \
    "$DEVICE:/usr/lib/liblvgl_examples.so*" \
    "$DEVICE:/usr/lib/liblvgl_demos.so*" \
    "$DEVICE:/usr/lib/libdrm.so*" \
    "$DEVICE:/usr/lib/libfreetype.so*" \
    "$DEVICE:/usr/lib/libavcodec.so*" \
    "$DEVICE:/usr/lib/libavformat.so*" \
    "$DEVICE:/usr/lib/libavutil.so*" \
    "$DEVICE:/usr/lib/libswscale.so*" \
    "$DEVICE:/usr/lib/libevdev.so*" \
    "$SYSROOT/lib/"

echo "Done. Build UI targets with -DK230_DEVICE_SYSROOT=$SYSROOT (the default)."
