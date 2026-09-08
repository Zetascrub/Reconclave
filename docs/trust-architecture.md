# Provisioned fleet trust

## Current deployment profile

The desktop is the primary coordinator (priority 100). The Cardputer ADV is a
portable secondary coordinator (priority 50), and the PoE-P4 is an execution
node. A device accepts a trusted capability only from a specifically
provisioned peer identity.

The development profile uses a different random 256-bit HMAC key for each
relationship: desktop to P4, Cardputer to P4, and desktop to Cardputer. There
is deliberately no fleet-wide command key. Compromise of one link does not
reveal either of the other links. Trusted requests bind the source,
destination, request, capability, target boot nonce, and a fresh request nonce.
This provides peer authentication, message integrity, boot-session binding,
and replay resistance. It does not encrypt HTTP traffic.

## Provisioning and flashing

Replace the synthetic example IDs below with your real device identities.
Generate or reproduce the ignored build inputs from the repository root:

```sh
python3 tools/provision_fleet.py \
  --desktop-id rc-desktop-example \
  --p4-id rc-p4-example \
  --cardputer-id rc-adv-example
```

The command creates `.reconclave-provisioning/fleet.json` with mode `0600` and
generates device headers under each firmware source tree. All three paths are
ignored by Git. Re-running preserves existing material; `--rotate` intentionally
creates an entirely new fleet generation and requires reflashing every member.
Never add the store or generated headers to source control or firmware logs.

The flasher-reported hardware MAC is the canonical embedded node suffix. Verify
the upload port and MAC immediately before each flash. A freshly flashed device
must announce `security.mode=provisioned-hmac-sha256-128`, `paired=true`, and a
new boot nonce before trusted commands are attempted.

## Coordinator selection

Priority declares the ownership order: desktop 100, Cardputer 50. Every trusted
request carries a signed priority and a bounded 15-second lease request. The P4
renews requests from the active owner, permits a higher-priority coordinator to
preempt, and rejects a different coordinator at equal or lower priority until
the lease expires. This lets the Cardputer take over when the desktop has been
absent for 15 seconds while making desktop recovery deterministic. Lease state
is intentionally volatile and clears on P4 restart; authentication is always
performed before lease selection, and a caller cannot claim a priority other
than the value compiled into its provisioned P4 trust record.

## Migration to production transport

Provisioned HMAC is an incremental compatibility profile, not the final trust
architecture. The production profile will give each device a unique asymmetric
key held in protected storage, issue a device certificate from a Reconclave
fleet CA, and require TLS 1.3 mutual authentication. Capability authorization
will remain independent of transport identity. The HMAC profile can then be
disabled after all deployed devices advertise mTLS support.

Acceptance requires certificate-chain and node-ID binding, revocation/rotation
tests, rejection of expired or unknown identities, encrypted packet-capture
verification, secure-boot and flash-encryption guidance, and a recovery path
that does not restore a shared fleet secret.

## Rollback and recovery

Keep the private fleet store backed up offline. A firmware rollback must use a
revision that understands the same provision generation. If any link key may be
exposed, rotate the complete generation and reflash all three devices; partial
rotation intentionally fails closed. Legacy Grove/NVS pairing remains available
for recovery experiments but is not authoritative when generated provisioning
is compiled into the current firmware.
