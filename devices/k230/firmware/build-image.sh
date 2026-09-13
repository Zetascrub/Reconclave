#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SDK_DIR="${1:-$SCRIPT_DIR/../../../.toolchains/k230_linux_sdk}"
DEFCONFIG="${2:-k230_canmv_t_display_rm69a10_defconfig}"

"$SCRIPT_DIR/check-host.sh"
if [[ ! -f "$SDK_DIR/buildroot-overlay/configs/$DEFCONFIG" ]]; then
	"$SCRIPT_DIR/prepare-sdk.sh" "$SDK_DIR"
fi
"$SCRIPT_DIR/install-to-sdk.sh" "$SDK_DIR" "$DEFCONFIG"

MAKE_ARGS=()
if [[ -n "${RECONCLAVE_HOST_CMAKE:-}" ]]; then
	[[ -x "$RECONCLAVE_HOST_CMAKE" ]] || {
		echo "RECONCLAVE_HOST_CMAKE is not executable: $RECONCLAVE_HOST_CMAKE" >&2
		exit 2
	}
	MAKE_ARGS+=("BR2_CMAKE=$RECONCLAVE_HOST_CMAKE" "BR2_CMAKE_HOST_DEPENDENCY=")
fi

make -C "$SDK_DIR" CONF="$DEFCONFIG" "$DEFCONFIG"
make -C "$SDK_DIR" CONF="$DEFCONFIG" "${MAKE_ARGS[@]}" all

IMAGE="$SDK_DIR/output/$DEFCONFIG/images/sysimage-sdcard.img"
[[ -f "$IMAGE" ]] || IMAGE="$SDK_DIR/output/$DEFCONFIG/images/sysimage-sdcard.img.gz"
[[ -f "$IMAGE" ]] || { echo "Build completed but no SD image was found." >&2; exit 1; }
echo "Firmware image: $IMAGE"
sha256sum "$IMAGE"
