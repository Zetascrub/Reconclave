#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../../.." && pwd)"
SDK_DIR="${1:-$PROJECT_DIR/.toolchains/k230_linux_sdk}"
BSP_DIR="${2:-$PROJECT_DIR/.toolchains/t-display-k230-bsp}"
SDK_URL="https://github.com/kendryte/k230_linux_sdk.git"
BSP_URL="https://github.com/Xinyuan-LilyGO/T-Display-K230.git"
SDK_COMMIT="22d02c6b6783a57a3aca7eb3160e313e772cb710"
BSP_TAG="v0.2.4"

clone_sparse() {
	local url="$1" destination="$2" revision="$3" paths="$4"
	git clone --filter=blob:none --no-checkout "$url" "$destination"
	git -C "$destination" sparse-checkout init --cone
	# shellcheck disable=SC2086
	git -C "$destination" sparse-checkout set $paths
	git -C "$destination" checkout "$revision"
}

if [[ ! -d "$SDK_DIR/.git" ]]; then
	mkdir -p "$(dirname "$SDK_DIR")"
	clone_sparse "$SDK_URL" "$SDK_DIR" "$SDK_COMMIT" \
		"buildroot-overlay tools src/little/uboot"
fi
if [[ ! -d "$BSP_DIR/.git" ]]; then
	mkdir -p "$(dirname "$BSP_DIR")"
	clone_sparse "$BSP_URL" "$BSP_DIR" "$BSP_TAG" \
		"k230_bsp"
fi

actual_sdk="$(git -C "$SDK_DIR" rev-parse HEAD)"
[[ "$actual_sdk" == "$SDK_COMMIT" ]] || {
	echo "SDK is at $actual_sdk; expected $SDK_COMMIT." >&2
	echo "Refusing to overwrite an existing SDK checkout." >&2
	exit 1
}

git -C "$SDK_DIR" sparse-checkout add tools src configs buildroot opensbi
git -C "$BSP_DIR" sparse-checkout add k230_bsp
"$BSP_DIR/k230_bsp/scripts/apply.sh" "$SDK_DIR"
"$SCRIPT_DIR/install-to-sdk.sh" "$SDK_DIR"

echo "K230 SDK prepared: $SDK_DIR"
