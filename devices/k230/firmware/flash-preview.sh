#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../../.." && pwd)"
ROOTFS=/run/media/tom/rootfs
BOOT=/run/media/tom/boot
DEVICE=/dev/sda
ROOTFS_UUID=c2ec81a4-cccd-4101-a680-552268f17a76
BOOT_UUID=e883b06a-6fa1-437d-8ed9-e90c24fbdc90

if [[ $EUID -ne 0 ]]; then
	echo "Run with sudo: sudo $0" >&2
	exit 2
fi

actual_rootfs="$(blkid -s UUID -o value "${DEVICE}2" 2>/dev/null || true)"
actual_boot="$(blkid -s UUID -o value "${DEVICE}1" 2>/dev/null || true)"
[[ "$actual_rootfs" == "$ROOTFS_UUID" && "$actual_boot" == "$BOOT_UUID" ]] || {
	echo "Refusing to flash: $DEVICE does not match the verified K230 card UUIDs." >&2
	exit 1
}
[[ "$(findmnt -n -o SOURCE --target "$ROOTFS")" == "${DEVICE}2" ]] || exit 1
[[ "$(findmnt -n -o SOURCE --target "$BOOT")" == "${DEVICE}1" ]] || exit 1

UI="$PROJECT_DIR/build-k230/devices/k230/reconclave_k230_ui"
NODE="$PROJECT_DIR/build-k230/devices/k230/reconclave_k230"
[[ -x "$UI" && -x "$NODE" ]] || {
	echo "Missing cross-built K230 binaries; run cmake --build build-k230 first." >&2
	exit 1
}

stamp="$(date -u +%Y%m%d-%H%M%S)"
backup="/mnt/Storage/k230-reconclave-preview-backup-$stamp.tar.gz"
paths=()
for path in \
	etc/init.d/S90reconclave \
	etc/init.d/S90reconclave-node \
	etc/init.d/S99reconclave-ui \
	etc/init.d/S99zz_k230_phone_ui \
	etc/hostname \
	etc/network/interfaces \
	usr/bin/reconclave-k230-node \
	usr/bin/reconclave-k230-ui \
	usr/sbin/reconclave-wifi \
	usr/sbin/reconclave-capture \
	usr/sbin/reconclave-screenshot \
	usr/sbin/reconclave-nmap \
	usr/sbin/reconclave-trust \
	usr/sbin/reconclave-status \
	usr/sbin/reconclave-ui; do
	[[ -e "$ROOTFS/$path" ]] && paths+=("$path")
done
tar -C "$ROOTFS" -czf "$backup" "${paths[@]}"
echo "Backup: $backup"
if [[ -f "$BOOT/logo.xrgb" ]]; then
	cp -a "$BOOT/logo.xrgb" "/mnt/Storage/k230-logo-pre-preview-$stamp.xrgb"
fi

install -D -m 0755 "$NODE" "$ROOTFS/usr/bin/reconclave-k230-node"
install -D -m 0755 "$UI" "$ROOTFS/usr/bin/reconclave-k230-ui"
install -D -m 0755 "$SCRIPT_DIR/rootfs-overlay/etc/init.d/S90reconclave-node" \
	"$ROOTFS/etc/init.d/S90reconclave-node"
install -D -m 0755 "$SCRIPT_DIR/rootfs-overlay/etc/init.d/S99reconclave-ui" \
	"$ROOTFS/etc/init.d/S99reconclave-ui"
install -D -m 0755 "$SCRIPT_DIR/rootfs-overlay/usr/sbin/reconclave-wifi" \
	"$ROOTFS/usr/sbin/reconclave-wifi"
install -D -m 0755 "$SCRIPT_DIR/rootfs-overlay/usr/sbin/reconclave-capture" \
	"$ROOTFS/usr/sbin/reconclave-capture"
install -D -m 0755 "$SCRIPT_DIR/rootfs-overlay/usr/sbin/reconclave-screenshot" \
	"$ROOTFS/usr/sbin/reconclave-screenshot"
install -D -m 0755 "$SCRIPT_DIR/rootfs-overlay/usr/sbin/reconclave-nmap" \
	"$ROOTFS/usr/sbin/reconclave-nmap"
install -D -m 0755 "$SCRIPT_DIR/rootfs-overlay/usr/sbin/reconclave-status" \
	"$ROOTFS/usr/sbin/reconclave-status"
install -D -m 0755 "$SCRIPT_DIR/rootfs-overlay/usr/sbin/reconclave-ui" \
	"$ROOTFS/usr/sbin/reconclave-ui"
install -D -m 0755 "$SCRIPT_DIR/rootfs-overlay/usr/sbin/reconclave-trust" \
	"$ROOTFS/usr/sbin/reconclave-trust"
install -m 0644 "$SCRIPT_DIR/rootfs-overlay/etc/network/interfaces" \
	"$ROOTFS/etc/network/interfaces"
install -m 0644 "$SCRIPT_DIR/rootfs-overlay/etc/hostname" "$ROOTFS/etc/hostname"

# Keep the stock launcher recoverable but outside BusyBox init's S* glob.
[[ ! -e "$ROOTFS/etc/init.d/S99zz_k230_phone_ui" ]] || \
	mv "$ROOTFS/etc/init.d/S99zz_k230_phone_ui" \
		"$ROOTFS/etc/init.d/disabled-S99zz_k230_phone_ui"
rm -f "$ROOTFS/etc/init.d/S90reconclave"

generated_logo="$PROJECT_DIR/.toolchains/k230_linux_sdk/buildroot-overlay/board/canaan/k230-soc/rootfs_overlay/logo.xrgb"
if [[ -f "$generated_logo" && "$(stat -c %s "$generated_logo")" == 2799104 ]]; then
	install -m 0644 "$generated_logo" "$BOOT/logo.xrgb"
fi

sync
sha256sum "$ROOTFS/usr/bin/reconclave-k230-node" \
	"$ROOTFS/usr/bin/reconclave-k230-ui" "$BOOT/logo.xrgb"
umount "${DEVICE}2"
umount "${DEVICE}1"
echo "Reconclave preview installed and $DEVICE safely unmounted."
