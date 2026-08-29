# Grove trust bootstrap

The v0.2 bootstrap uses the Cardputer–P4 Grove UART as a physically trusted
channel. Pressing `P` on the Cardputer generates a random 256-bit peer secret,
transfers it directly to the addressed P4, and stores the accepted trust record
in NVS on both devices.

The secret is never sent over Ethernet or Wi-Fi. Network requests and responses
carry truncated HMAC-SHA256 authentication over both node identities, request
identity/status, capability, a random request nonce, and the P4's random boot
challenge. The P4 rejects duplicate nonces during the current boot. A new boot
challenge invalidates traffic captured before a P4 restart without requiring
wall-clock synchronisation.

This is a pre-production shared-secret trust profile. A later protocol revision
can replace the Grove transfer with persistent asymmetric device identities
without changing the physical pairing workflow. NVS encryption, secure boot,
and flash encryption remain deployment-hardening tasks.

## Operator flow

1. Power both devices and connect Grove.
2. Wait for `Grove connected` on the Cardputer.
3. Press `P` once and confirm `Trust saved to NVS`.
4. Press `R` to refresh the P4 network announcement.
5. Press `Enter`; only an authenticated response is displayed.
6. Disconnect Grove and power-cycle both devices.
7. Discover and request information again; no re-pairing should be required.

The P4 intentionally refuses replacement by a different peer/secret. Recovery
currently requires erasing the `rc_trust` NVS namespace during development; a
physical long-hold reset workflow will be added before general release.
