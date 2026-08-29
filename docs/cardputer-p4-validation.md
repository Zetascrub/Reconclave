# Cardputer ADV + PoE-P4 validation

Use this checklist for the first hardware run. It intentionally verifies only
the unauthenticated, read-only v0.1 slice.

## Preparation

- Build both targets from clean build directories.
- Confirm each serial device before flashing.
- Connect the P4 Ethernet/PoE side and the Cardputer to the same LAN/VLAN.
- Keep serial monitors open for both devices during the first boot.

## P4 checks

1. Flash and boot the P4 without Ethernet. The LED should settle blue.
2. Attach Ethernet. It should pass through amber and settle green after DHCP.
3. Confirm the log prints `Reconclave 0.1.0 node rc-p4-... started`.
4. Confirm the log prints its HTTP address after DHCP.
5. From a host on the LAN, fetch `/reconclave/v1/announce` and confirm the
   envelope identifies a `poe-p4` node with `system.info`.

## Cardputer checks

1. Flash and boot the Cardputer. Enter a test-network SSID and password.
2. Reboot and confirm credentials persist and reconnect automatically.
3. Confirm the dashboard labels the Cardputer `node + coordinator`.
4. Press `R`; confirm the P4 appears by ID, IP, and firmware.
5. Press `Enter`; confirm heap and uptime appear from the remote response.
6. Power off/unplug the P4 and confirm it expires after roughly 45 seconds.
7. Restore the P4 and confirm it is rediscovered.
8. Press `W`, change networks, and verify the replacement credentials persist.

## Expected limitations

- There is no trusted-node pairing or message authentication yet.
- Discovery depends on multicast DNS, with hostname fallback for the P4.
- Only one P4 is represented in the initial Cardputer dashboard.
- Job events, evidence persistence, and WebSocket streaming are not enabled.
- Do not enable assessment or mutation capabilities in this firmware.

Record serial logs and any reset reasons for failures. A successful run proves
the v0.1 discovery → announcement → capability request → response path on two
different hardware platforms.
