#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../../.." && pwd)"
SDK_DIR="${1:-}"
DEFCONFIG="${2:-k230_canmv_t_display_rm69a10_defconfig}"
MASCOT_DIR="${RECONCLAVE_MASCOT_DIR:-/mnt/Storage/Coding/Misc/Mascot}"

if [[ -z "$SDK_DIR" || ! -f "$SDK_DIR/Makefile" ]]; then
	echo "Usage: $0 /path/to/k230_linux_sdk [defconfig]" >&2
	exit 2
fi
SDK_DIR="$(cd "$SDK_DIR" && pwd)"

PACKAGE_DIR="$SDK_DIR/buildroot-overlay/package/reconclave-k230"
ROOTFS_DIR="$SDK_DIR/buildroot-overlay/board/canaan/k230-soc/rootfs_overlay"
CONFIG_IN="$SDK_DIR/buildroot-overlay/package/Config_canaan.in"
DEFCONFIG_PATH="$SDK_DIR/buildroot-overlay/configs/$DEFCONFIG"

# The vendor defconfig hard-codes /opt/toolchain. Permit a relocatable SDK
# checkout to point at an existing Xuantie bundle without requiring root or a
# host-wide /opt symlink.
if [[ -n "${RECONCLAVE_K230_TOOLCHAIN:-}" ]]; then
	toolchain_path="$(cd "$RECONCLAVE_K230_TOOLCHAIN" && pwd)/"
	sed -i "s|^BR2_TOOLCHAIN_EXTERNAL_PATH=.*|BR2_TOOLCHAIN_EXTERNAL_PATH=\"$toolchain_path\"|" \
		"$DEFCONFIG_PATH"
fi

[[ -f "$CONFIG_IN" && -f "$DEFCONFIG_PATH" ]] || {
	echo "The LILYGO K230 BSP overlay must be applied before Reconclave." >&2
	exit 1
}

mkdir -p "$PACKAGE_DIR/src"
install -m 0644 "$SCRIPT_DIR/Config.in" "$PACKAGE_DIR/Config.in"
install -m 0644 "$SCRIPT_DIR/reconclave-k230.mk" "$PACKAGE_DIR/reconclave-k230.mk"
rsync -a --delete \
	--exclude '.git' --exclude '.toolchains' --exclude 'build*' \
	--exclude '.venv*' --exclude '.cache' \
	"$PROJECT_DIR/" "$PACKAGE_DIR/src/"
rsync -a --exclude 'root/.ssh/' "$SCRIPT_DIR/rootfs-overlay/" "$ROOTFS_DIR/"
chmod 0755 "$ROOTFS_DIR/etc/init.d/S90reconclave-node" \
	"$ROOTFS_DIR/etc/init.d/S99reconclave-ui" \
	"$ROOTFS_DIR/etc/init.d/S55reconclave-ssh-client" \
	"$ROOTFS_DIR/usr/sbin/reconclave-wifi" \
	"$ROOTFS_DIR/usr/sbin/reconclave-status" \
	"$ROOTFS_DIR/usr/sbin/reconclave-ui" \
	"$ROOTFS_DIR/usr/sbin/reconclave-capture" \
	"$ROOTFS_DIR/usr/sbin/reconclave-screenshot" \
	"$ROOTFS_DIR/usr/sbin/reconclave-nmap" \
	"$ROOTFS_DIR/usr/sbin/reconclave-trust"
# Root SSH key material is per-deployment and gitignored (public-source policy,
# tools/check_public_tree.py), so it is not carried by the rsync above. Assemble
# it here from the operator's local files, failing loudly rather than building a
# key-only-login image (PasswordAuthentication no) with no authorized key.
OVERLAY_SSH="$SCRIPT_DIR/rootfs-overlay/root/.ssh"
if [[ ! -s "$OVERLAY_SSH/authorized_keys" ]]; then
	echo "Missing $OVERLAY_SSH/authorized_keys" >&2
	echo "This image uses key-only root login; a build without it would lock you out." >&2
	echo "Copy devices/k230/firmware/ssh/authorized_keys.example there and add your public key(s)." >&2
	exit 1
fi
install -D -m 0600 "$OVERLAY_SSH/authorized_keys" "$ROOTFS_DIR/root/.ssh/authorized_keys"
# Client config carries no secret; use the operator's copy if present, else the template.
if [[ -f "$OVERLAY_SSH/config" ]]; then
	install -D -m 0600 "$OVERLAY_SSH/config" "$ROOTFS_DIR/root/.ssh/config"
else
	install -D -m 0600 "$SCRIPT_DIR/ssh/config" "$ROOTFS_DIR/root/.ssh/config"
fi
chmod 0700 "$ROOTFS_DIR/root/.ssh"
chmod 0600 "$ROOTFS_DIR/etc/ssh/sshd_config"
rm -f "$ROOTFS_DIR/etc/init.d/S99zz_k230_phone_ui"

if ! grep -q 'package/reconclave-k230/Config.in' "$CONFIG_IN"; then
	printf '\nsource "package/reconclave-k230/Config.in"\n' >>"$CONFIG_IN"
fi
if grep -q '^# BR2_PACKAGE_RECONCLAVE_K230 is not set' "$DEFCONFIG_PATH"; then
	sed -i 's/^# BR2_PACKAGE_RECONCLAVE_K230 is not set/BR2_PACKAGE_RECONCLAVE_K230=y/' "$DEFCONFIG_PATH"
elif ! grep -q '^BR2_PACKAGE_RECONCLAVE_K230=y' "$DEFCONFIG_PATH"; then
	printf '\nBR2_PACKAGE_RECONCLAVE_K230=y\n' >>"$DEFCONFIG_PATH"
fi
# Reconclave replaces, rather than competes with, the vendor launcher for DRM.
sed -i 's/^BR2_PACKAGE_K230_PHONE_UI=y/# BR2_PACKAGE_K230_PHONE_UI is not set/' "$DEFCONFIG_PATH"

# Field-analysis utilities selected for bounded, evidence-producing workflows.
# Wireless injection suites are intentionally omitted until monitor mode is
# demonstrated on the AIC8800 driver used by this board.
for option in \
	BR2_PACKAGE_LIBPCAP \
	BR2_PACKAGE_TCPDUMP \
	BR2_PACKAGE_NMAP \
	BR2_PACKAGE_OPENSSL \
	BR2_PACKAGE_LIBOPENSSL_BIN \
	BR2_PACKAGE_SOCAT \
	BR2_PACKAGE_ETHTOOL
do
	if grep -q "^# $option is not set" "$DEFCONFIG_PATH"; then
		sed -i "s/^# $option is not set/$option=y/" "$DEFCONFIG_PATH"
	elif ! grep -q "^$option=y" "$DEFCONFIG_PATH"; then
		printf '%s=y\n' "$option" >>"$DEFCONFIG_PATH"
	fi
done

for setting in 'BR2_TARGET_TZ_INFO=y' 'BR2_TARGET_TZ_ZONELIST="europe"' \
	'BR2_TARGET_LOCALTIME="Europe/London"'; do
	key=${setting%%=*}
	sed -i "/^${key}=/d;/^# ${key} is not set/d" "$DEFCONFIG_PATH"
	printf '%s\n' "$setting" >>"$DEFCONFIG_PATH"
done

SPLASH="$MASCOT_DIR/Splash-screen-568×1232-landscape.png"
if [[ -f "$SPLASH" ]] && command -v magick >/dev/null 2>&1; then
	# U-Boot consumes a headerless native-portrait 568x1232 BGRX framebuffer.
	magick "$SPLASH" -rotate 270 -resize 568x1232! -alpha off -depth 8 \
		BGRA:"$ROOTFS_DIR/logo.xrgb"
	actual="$(stat -c %s "$ROOTFS_DIR/logo.xrgb")"
	[[ "$actual" == 2799104 ]] || { echo "Unexpected splash size: $actual" >&2; exit 1; }
else
	echo "Warning: mascot splash or ImageMagick missing; keeping BSP boot logo." >&2
fi

# Force Buildroot to resynchronise local overlay/package content.
rm -rf "$SDK_DIR/output/$DEFCONFIG/build/reconclave-k230-"* \
	"$SDK_DIR/output/$DEFCONFIG/target/usr/bin/reconclave-k230-"* 2>/dev/null || true
find "$SDK_DIR/output" -maxdepth 2 -name .overlay_sync -type f -delete 2>/dev/null || true

echo "Reconclave firmware layer installed into $SDK_DIR"
echo "Build with: make -C '$SDK_DIR' CONF='$DEFCONFIG' '$DEFCONFIG' all"
