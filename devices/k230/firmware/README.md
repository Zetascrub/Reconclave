# Reconclave K230 firmware image

This layer turns LILYGO's K230 Linux BSP into a Reconclave appliance image.
It retains the board-specific U-Boot, Linux, NuttX-side services, DRM/touch,
camera, audio, Wi-Fi/Bluetooth, charger and GPIO support, while replacing the
phone launcher with the Reconclave UI and protocol node.

The installer is deliberately separate from flashing. It never writes a block
device. Build an image with:

```sh
devices/k230/firmware/build-image.sh .toolchains/k230_linux_sdk
```

On hosts whose system CMake is newer than Buildroot supports, point the build
at a compatible standalone CMake (3.18 through 3.31 is suitable here):

```sh
RECONCLAVE_HOST_CMAKE=/path/to/cmake-3.31/bin/cmake \
  devices/k230/firmware/build-image.sh .toolchains/k230_linux_sdk
```

Prerequisites:

For a new workspace, `prepare-sdk.sh` sparsely clones the pinned SDK and BSP,
checks their revisions, applies the board overlay and installs Reconclave:

```sh
devices/k230/firmware/prepare-sdk.sh
```

Existing SDK checkouts are never reset or overwritten. Run `check-host.sh` for
a quick dependency and known-version check before a long build. Known-broken
host combinations fail immediately instead of wasting time compiling host
tools. `RECONCLAVE_ALLOW_UNSUPPORTED_HOST=1` is available for deliberate
experimentation.

`install-to-sdk.sh` stages the current Reconclave source as a local Buildroot
package, installs the two init services, disables the competing vendor launcher,
and converts the mascot landscape splash into U-Boot's native 568x1232 BGRX
framebuffer. Set `RECONCLAVE_MASCOT_DIR` to override the default artwork path.

The resulting image remains recoverable with the original full-card and raw
U-Boot-region backups. Flashing is intentionally a separate, explicit operator
step because it destroys the destination card's current contents.

## First boot and networking

Wired Ethernet uses DHCP automatically and is the recovery/initial-setup path.
After connecting over Ethernet, install a persistent WPA2 Wi-Fi profile without
placing the password in shell history:

```sh
reconclave-wifi "network name"
```

The helper prompts without echo, writes a root-only wpa_supplicant profile and
brings `wlan0` up immediately. The stock firmware's U-Boot-environment password
storage and password-logging `sta.sh` workflow are intentionally not retained.

Run `reconclave-status` for a compact first-boot report covering addresses,
default route, Reconclave services, storage, kernel power-supply data and recent
service errors.

Provision the execution trust domain interactively after connecting. The
passphrase is entered twice without echo and only its SHA-256-derived key is
stored with root-only permissions:

```sh
reconclave-trust provision
```

Use the same passphrase on the Reconclave coordinator. Until this step is
complete, the node advertises only its public system and passive ARP
capabilities; assessment tools remain unavailable by design.

OpenSSH client and server are included. This personal appliance image installs
the owner's Ed25519 public key(s) and replaces the BSP's unsafe empty-root-
password configuration with key-only root administration. Password, empty-
password, keyboard-interactive, agent-forwarding, TCP-forwarding, X11 and
tunnel access are disabled.

The root `authorized_keys` is **per-deployment key material and is not committed**
(public-source policy, `tools/check_public_tree.py`, which rejects any `.ssh/`
path). Before building, copy `ssh/authorized_keys.example` to
`rootfs-overlay/root/.ssh/authorized_keys` (gitignored) and add your public
key(s); `install-to-sdk.sh` assembles it into the image and **fails the build if
it is missing**, since a key-only-login image with no authorized key would lock
you out. The generic client `config` ships from `ssh/config` unless a local
override is present. `ssh`, `scp`, `sftp`, `ssh-keygen` and
`ssh-keyscan` remain available locally for authorised field workflows.
The device generates a separate Ed25519 outbound client identity on first boot;
its public key is returned by `system.ssh.status` so a coordinator can arrange
authorisation without exposing private key material. Outbound SSH defaults to
key-only authentication and strict host-key verification.

## Current validation

- Shell syntax, source staging, init-service modes and splash byte size pass.
- Both Reconclave K230 binaries build and install through the pinned Buildroot
  SDK with LVGL and libdrm enabled and the vendor launcher disabled.
- A complete image was built in Kendryte's `ghcr.io/kendryte/k230_sdk:latest`
  container with the pinned Xuantie V3.0.2 toolchain. For LVGL's configure step,
  `pcpp` 1.30 was supplied as a local wheel because the SDK's host Python is
  intentionally built without SSL support.
- The generated 728 MiB raw SD image has a valid DOS partition table, with an
  80 MiB boot filesystem and 600 MiB root filesystem. Both ext4 filesystems and
  the compressed image pass their integrity checks.
- The final root filesystem contains the Reconclave node and UI binaries, both
  executable init services, and the corrected upright 2,799,104-byte splash.
