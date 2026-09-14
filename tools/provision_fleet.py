#!/usr/bin/env python3
"""Generate per-link Reconclave development trust material for known devices.

Private material is written only to ignored paths. Re-running without --rotate
preserves the existing trust store so ordinary firmware rebuilds retain identity.
"""

from __future__ import annotations

import argparse
import json
import secrets
import os
import re
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
STORE = ROOT / ".reconclave-provisioning" / "fleet.json"
# Device projects are sibling repositories after the monorepo extraction.
P4_HEADER = ROOT.parent / "Relay" / "main" / "generated_trust.h"
CARD_HEADER = ROOT.parent / "FieldDeck" / "src" / "generated_trust.h"


def private_write(path: Path, text: str) -> None:
    """Replace private material without a world-readable intermediate file."""
    fd, temp = tempfile.mkstemp(prefix=".trust-", dir=path.parent)
    try:
        with os.fdopen(fd, "w") as stream:
            stream.write(text)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temp, path)
    finally:
        if os.path.exists(temp):
            os.unlink(temp)


def key_bytes(value: str) -> str:
    return ", ".join(f"0x{value[index:index + 2]}" for index in range(0, len(value), 2))


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--desktop-id", required=True)
    parser.add_argument("--p4-id", required=True)
    parser.add_argument("--cardputer-id", required=True)
    parser.add_argument("--rotate", action="store_true")
    args = parser.parse_args()
    ids = (args.desktop_id, args.p4_id, args.cardputer_id)
    if len(set(ids)) != 3 or any(not re.fullmatch(r"[A-Za-z0-9_-]{1,64}", value) for value in ids):
        parser.error("device IDs must be distinct, 1-64 letters/digits/underscore/hyphen")
    STORE.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
    if STORE.exists() and not args.rotate:
        fleet = json.loads(STORE.read_text())
        if set(fleet.get("devices", {})) != set(ids):
            parser.error("existing fleet IDs differ; use its original IDs or an intentional --rotate")
        pairs = (f"{ids[0]}|{ids[1]}", f"{ids[2]}|{ids[1]}", f"{ids[0]}|{ids[2]}")
        values = [fleet.get("links", {}).get(pair, "") for pair in pairs]
        values += list(fleet.get("storage_keys", {}).values())
        if any(not isinstance(value, str) or not re.fullmatch(r"[0-9a-f]{64}", value) for value in values):
            parser.error("existing trust store has invalid keys; restore a valid backup")
    else:
        fleet = {
            "version": 1,
            "devices": {
                args.desktop_id: {"role": "coordinator", "priority": 100},
                args.cardputer_id: {"role": "coordinator", "priority": 50},
                args.p4_id: {"role": "node", "priority": 0},
            },
            "links": {
                f"{args.desktop_id}|{args.p4_id}": secrets.token_hex(32),
                f"{args.cardputer_id}|{args.p4_id}": secrets.token_hex(32),
                f"{args.desktop_id}|{args.cardputer_id}": secrets.token_hex(32),
            },
        }
    # Per-device at-rest storage key: unlike the pairwise links above, this protects a
    # device's own local evidence storage (poe-p4's NVS outbox blob, cardputer-adv's SD
    # card evidence log) and so must stay stable across re-pairing with any coordinator,
    # never derived from or tied to a specific link. Backfilled on an ordinary
    # (non---rotate) run against an older store that predates this field, so upgrading
    # doesn't force re-pairing every device.
    storage_keys = fleet.setdefault("storage_keys", {})
    for device_id in (args.p4_id, args.cardputer_id):
        storage_keys.setdefault(device_id, secrets.token_hex(32))
    private_write(STORE, json.dumps(fleet, indent=2) + "\n")
    STORE.chmod(0o600)
    links = fleet["links"]
    desktop_p4 = links[f"{args.desktop_id}|{args.p4_id}"]
    card_p4 = links[f"{args.cardputer_id}|{args.p4_id}"]
    desktop_card = links[f"{args.desktop_id}|{args.cardputer_id}"]
    p4_storage = storage_keys[args.p4_id]
    card_storage = storage_keys[args.cardputer_id]
    private_write(P4_HEADER,
        "#pragma once\n#include <stdint.h>\n"
        "typedef struct { const char *peer_id; uint8_t priority; uint8_t key[32]; } rc_provisioned_peer_t;\n"
        "static const rc_provisioned_peer_t RC_PROVISIONED_PEERS[] = {\n"
        f'  {{"{args.desktop_id}", 100, {{{key_bytes(desktop_p4)}}}}},\n'
        f'  {{"{args.cardputer_id}", 50, {{{key_bytes(card_p4)}}}}},\n'
        "};\n"
        "// At-rest AES-256-GCM key for this device's own local evidence storage --\n"
        "// independent of any coordinator pairing above. See encrypted_spool.py's\n"
        "// EncryptedSpool(key=...) for the matching desktop-side reader.\n"
        f"static const uint8_t RC_STORAGE_KEY[32] = {{{key_bytes(p4_storage)}}};\n")
    private_write(CARD_HEADER,
        "#pragma once\n#include <stdint.h>\n"
        f'static constexpr char RC_PROVISIONED_P4_ID[] = "{args.p4_id}";\n'
        f"static constexpr uint8_t RC_PROVISIONED_P4_KEY[32] = {{{key_bytes(card_p4)}}};\n"
        f'static constexpr char RC_PROVISIONED_PRIMARY_ID[] = "{args.desktop_id}";\n'
        f"static constexpr uint8_t RC_PROVISIONED_PRIMARY_KEY[32] = {{{key_bytes(desktop_card)}}};\n"
        "static constexpr uint8_t RC_COORDINATOR_PRIORITY = 50;\n"
        "// At-rest AES-256-GCM key for this device's own local evidence storage --\n"
        "// independent of any coordinator pairing above. See encrypted_spool.py's\n"
        "// EncryptedSpool(key=...) for the matching desktop-side reader.\n"
        f"static constexpr uint8_t RC_STORAGE_KEY[32] = {{{key_bytes(card_storage)}}};\n")
    print(f"Trust store: {STORE}")
    print(f"P4 header: {P4_HEADER}")
    print(f"Cardputer header: {CARD_HEADER}")


if __name__ == "__main__":
    main()
