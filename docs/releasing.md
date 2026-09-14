# Releases, signing and private deployment keys

The initial public release is a **source-only development preview**. Do not
publish binaries produced by a provisioned local build. No workflow in this
repository automatically publishes releases or changes repository visibility.

The standalone device repos (FieldDeck, ZetaDongle, Relay, Reconclave Command)
automate a narrower, separate thing on a version tag push: FieldDeck, ZetaDongle
and Relay publish an **unprovisioned example firmware build** (see "Public
example firmware" below); Reconclave Command publishes a convenience source +
pre-built web bundle. Neither is the maintainer's Ed25519-signed formal source
release described later in this document, and none of it touches a provisioned
deployment key.

## Three different kinds of trust

| Mechanism | What it protects | Current state |
|---|---|---|
| Pairwise fleet HMAC keys | Authorised commands and replay protection | Generated per deployment; embedded in private builds |
| Ed25519 release key | Offline artifact authenticity, hash, target and version | Implemented by tools/release_signing.py |
| ESP Secure Boot / flash encryption | Device boot verification / resistance to flash readout | Not enabled by this release setup |

A valid detached signature does not make a deployment binary safe to share,
encrypt it, enforce rollback policy or make the device verify its publisher.
The existing OTA HMAC checks are separate from the Ed25519 release verifier.

## Maintainer signing key

Generate once, using a location outside the checkout (POSIX host):

```sh
.venv/bin/python tools/release_signing.py keygen \
  --private-key "$HOME/.local/share/reconclave/signing/release-ed25519.pem" \
  --public-key release/keys/reconclave-release.pub.pem
```

The private key is an unencrypted PKCS8 PEM file with owner-only permissions;
the tool creates its directory with mode 0700 and key with mode 0600. Re-running
preserves it and refuses to replace a different public key. Do not commit it,
put it in a firmware header, or upload it to Actions. Disk encryption and an
encrypted offline backup are the maintainer's responsibility. No offline backup
is created automatically. A lost private key cannot be recreated from the public
key. Back it up before relying on this identity for releases.

The checked-in public key's SHA-256 fingerprint (raw Ed25519 key bytes) is:

```
af9c2ab8bba500f5f540610e20081bd186004d2ccd8342180eb90789ebb4c782
```

Communicate the fingerprint through a separately trusted channel. Downloading
an artifact and an attacker-replaced public key together does not authenticate
the publisher. Rotation requires a new identity and an authenticated notice;
retain old public keys for existing releases and clearly mark revoked identities.
Never reuse a fleet key as a release key.

## Source release

Commit the intended source first. The packaging command requires a clean tree,
a licence decision, reporting/contribution files and the public key. It blocks
known secret paths, deployment binaries, symlinks and private PEM material.
Run the full CI suite and Gitleaks history scan on the release commit; the path
gate is not a general secret detector.

```sh
mkdir -p .reconclave-releases
.venv/bin/python tools/source_release.py --version 0.1.0-preview.1 \
  --output .reconclave-releases/Reconclave-0.1.0-preview.1.tar.gz
.venv/bin/python tools/release_signing.py sign \
  --artifact .reconclave-releases/Reconclave-0.1.0-preview.1.tar.gz \
  --manifest .reconclave-releases/Reconclave-0.1.0-preview.1.json \
  --private-key "$HOME/.local/share/reconclave/signing/release-ed25519.pem" \
  --target source --kind source --version 0.1.0-preview.1 \
  --commit "$(git rev-parse HEAD)"
.venv/bin/python tools/release_signing.py verify \
  --artifact .reconclave-releases/Reconclave-0.1.0-preview.1.tar.gz \
  --manifest .reconclave-releases/Reconclave-0.1.0-preview.1.json \
  --public-key release/keys/reconclave-release.pub.pem \
  --target source --version 0.1.0-preview.1
```

Manually inspect the archive, then publish only the archive, its signed manifest
and the public key. The signature binds filename, byte size, SHA-256, source
commit and dirty-source flag, release version, target and artifact classification. The source commit
is a maintainer assertion, not a reproducible-build attestation. Users should
supply their expected target/version to verification and only use an already
trusted public key. Verification never flashes or executes the artifact.

## Private firmware signing

Build with the intended deployment's ignored trust headers. Use the same
sign/verify commands with `--target cardputer-adv` or `--target poe-p4`, and
`--kind private-deployment-firmware`. Keep the image and manifest private. No
public *live-functional* generic binary mode exists yet: that would require
runtime per-device provisioning without shared embedded secrets and hardware
validation. What does exist, and is intentionally different, is the
dummy-trust example firmware described next — it is public and generic
precisely because it is not live-functional against a real fleet.

Private validation builds made before committing may use `--dirty-source`; this
state is covered by the signature and shown by verification. Public source
releases reject that flag. Such a manifest identifies a development worktree,
not a reproducible release of the stated base commit.

## Public example firmware (dummy-trust CI builds)

FieldDeck, ZetaDongle and Relay each keep a disposable, all-zero trust header
under `config/generated_trust.ci.h` — "never provision hardware with this
file" — used to compile-check the firmware in CI without any real fleet key
present. Since 2026-09, pushing a version tag (`vX.Y.Z`) to one of those
standalone repos also builds firmware against that same disposable header and
publishes it as a public GitHub Release, labelled an **unprovisioned example
build**.

This is deliberately not the same thing "no workflow automatically publishes
releases" (above) warns against, because the artifact contains no deployment
secret: HMAC and storage keys are all-zero, peer IDs are `ci-*` placeholders.
It cannot authenticate against a real coordinator or peer, and any data it
would encrypt uses a publicly-known key. It exists purely so someone can flash
a device, confirm it boots and runs the base app, and then provision it for
real with `tools/provision_fleet.py` before pointing it at a live fleet — the
release notes on each of these say so explicitly.

This mode is unsigned (no Ed25519 manifest) and unrelated to the maintainer
signing key above; it runs entirely inside each device repo's own
`release.yml` with the repo-scoped `GITHUB_TOKEN`, no private key involved.
Reconclave Command's tag-triggered release is a separate, unsigned convenience
package (source + pre-built web UI) for the same reason it carries no trust
header at all: its keys are supplied at runtime via the environment, never
baked into a build.

A **provisioned** build — real per-device keys copied in from
`tools/provision_fleet.py` — must never be built or published by one of these
workflows. Nothing about this section changes that: private firmware signing
above remains the only path for a build meant to actually join a fleet.

## Secure Boot is a separate device migration

Do not burn eFuses based on detached release signing. ESP32-S3 Secure Boot v2
uses a different signing format/key scheme from this Ed25519 tool. Before a
production migration, validate the pinned toolchain, signed bootloader and app,
OTA verification and rollback, per-device encryption keys, recovery transport,
key rotation and rejected-image behaviour on a spare board. Document the
irreversible settings and a tested recovery method before applying them to the
working fleet. P4 needs its own chip/toolchain validation.

[Espressif Secure Boot documentation](https://docs.espressif.com/projects/esp-idf/en/v4.4.6/esp32s3/security/secure-boot-v2.html)
explains the eFuse trust anchor and restrictions. This repository preparation
runs no eFuse commands and changes no deployed keys.

## Visibility checklist

- Code uses MIT; include LICENSE and the separate reserved artwork terms.
  Confirm that distributed assets are owned or appropriately authorised.
- Commit reviewed changes; pass CI and secret scans on that exact commit.
- Review historical Actions logs and public-facing metadata/screenshots.
- Back up the signing key privately; distribute its fingerprint separately.
- Confirm the archive contains no generated headers, evidence or binaries.
- On the explicitly authorised visibility change, enable GitHub private
  vulnerability reporting and verify its form; enable available secret scanning
  and push protection. They are not assumed to be active while the repo is private.
- Publish a source preview with known limitations, not production guarantees.
