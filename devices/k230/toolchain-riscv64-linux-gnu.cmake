# CMake toolchain file for cross-compiling the K230 native app.
#
# Targets the glibc riscv64 userland already flashed on stock LILYGO
# T-Display K230 / Canaan CanMV-K230 images (verified against libc-2.33.so
# and ld-linux-riscv64-lp64d.so.1 on-device). Matches the Xuantie-900
# gcc-linux-glibc toolchain shipped by kendryte/k230_sdk.
#
# Usage:
#   cmake -B build-k230 -S . \
#     -DCMAKE_TOOLCHAIN_FILE=devices/k230/toolchain-riscv64-linux-gnu.cmake \
#     -DRECONCLAVE_BUILD_K230=ON -DRECONCLAVE_BUILD_TESTS=OFF
#
# Requires the toolchain's bin/ directory on PATH, or set
# K230_TOOLCHAIN_PREFIX to an absolute path prefix
# (e.g. /opt/toolchain/Xuantie-900-gcc-linux-5.10.4-glibc-x86_64-V2.6.0/bin/riscv64-unknown-linux-gnu-).

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR riscv64)

if(NOT DEFINED K230_TOOLCHAIN_PREFIX)
  set(K230_TOOLCHAIN_PREFIX "riscv64-unknown-linux-gnu-")
endif()

set(CMAKE_C_COMPILER   "${K230_TOOLCHAIN_PREFIX}gcc")
set(CMAKE_CXX_COMPILER "${K230_TOOLCHAIN_PREFIX}g++")

# The Xuantie toolchain defaults to -march=rv64gcxthead, which selects the
# lib64xthead multilib and links against ld-linux-riscv64xthead-lp64d.so.1.
# The device's actual rootfs only ships the plain (non-xthead)
# ld-linux-riscv64-lp64d.so.1 (confirmed against /lib/busybox on-device), so
# force the standard rv64gc/lp64d multilib instead.
set(CMAKE_C_FLAGS_INIT   "-march=rv64gc -mabi=lp64d")
set(CMAKE_CXX_FLAGS_INIT "-march=rv64gc -mabi=lp64d")

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# UI targets (LVGL + DRM) link against the *actual* on-device shared
# libraries rather than a self-built LVGL, since k230_phone_ui already
# ships a proven, working liblvgl.so/libdrm.so on this exact hardware -
# see devices/k230/README.md's UI section. This sysroot is populated by
# pulling those .so files plus matching upstream headers (LVGL v9.5.0
# source, libdrm-2.4.124 source) and the Buildroot package's own
# lv_conf.h (kendryte/k230_sdk src/little/buildroot-ext/package/lvgl);
# not committed to the repo (large, device-specific binaries) - see
# devices/k230/README.md for how to regenerate it.
if(NOT DEFINED K230_DEVICE_SYSROOT)
  set(K230_DEVICE_SYSROOT "${CMAKE_CURRENT_LIST_DIR}/../../.toolchains/device-sysroot")
endif()
set(CMAKE_FIND_ROOT_PATH "${K230_DEVICE_SYSROOT}")

# liblvgl.so's own transitive dependencies (ffmpeg, freetype's libpng,
# OpenSSL, ...) go many layers deep and are all genuinely present and
# resolvable by the real device's dynamic linker (same rootfs they came
# from) - allow-shlib-undefined trusts that at link time instead of
# requiring every transitive .so pulled into this sysroot too.
set(CMAKE_EXE_LINKER_FLAGS_INIT "-Wl,--allow-shlib-undefined")
