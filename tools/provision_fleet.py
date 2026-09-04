#!/usr/bin/env python3
"""Generate per-link Reconclave development trust material for known devices.

Private material is written only to ignored paths. Re-running without --rotate
preserves the existing trust store so ordinary firmware rebuilds retain identity.
"""

from __future__ import annotations

import argparse
import json
import secrets
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
STORE = ROOT / ".reconclave-provisioning" / "fleet.json"
P4_HEADER = ROOT / "devices/poe-p4/main/generated_trust.h"
CARD_HEADER = ROOT / "devices/cardputer-adv/src/generated_trust.h"


def key_bytes(value: str) -> str:
    return ", ".join(f"0x{value[index:index + 2]}" for index in range(0, len(value), 2))


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--desktop-id", required=True)
    parser.add_argument("--p4-id", required=True)
    parser.add_argument("--cardputer-id", required=True)
    parser.add_argument("--rotate", action="store_true")
    args = parser.parse_args()
    STORE.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
    if STORE.exists() and not args.rotate:
        fleet = json.loads(STORE.read_text())
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
        STORE.write_text(json.dumps(fleet, indent=2) + "\n")
        STORE.chmod(0o600)
    links = fleet["links"]
    desktop_p4 = links[f"{args.desktop_id}|{args.p4_id}"]
    card_p4 = links[f"{args.cardputer_id}|{args.p4_id}"]
    desktop_card = links[f"{args.desktop_id}|{args.cardputer_id}"]
    P4_HEADER.write_text(
        "#pragma once\n#include <stdint.h>\n"
        "typedef struct { const char *peer_id; uint8_t priority; uint8_t key[32]; } rc_provisioned_peer_t;\n"
        "static const rc_provisioned_peer_t RC_PROVISIONED_PEERS[] = {\n"
        f'  {{"{args.desktop_id}", 100, {{{key_bytes(desktop_p4)}}}}},\n'
        f'  {{"{args.cardputer_id}", 50, {{{key_bytes(card_p4)}}}}},\n'
        "};\n")
    CARD_HEADER.write_text(
        "#pragma once\n#include <stdint.h>\n"
        f'static constexpr char RC_PROVISIONED_P4_ID[] = "{args.p4_id}";\n'
        f"static constexpr uint8_t RC_PROVISIONED_P4_KEY[32] = {{{key_bytes(card_p4)}}};\n"
        f'static constexpr char RC_PROVISIONED_PRIMARY_ID[] = "{args.desktop_id}";\n'
        f"static constexpr uint8_t RC_PROVISIONED_PRIMARY_KEY[32] = {{{key_bytes(desktop_card)}}};\n"
        "static constexpr uint8_t RC_COORDINATOR_PRIORITY = 50;\n")
    print(f"Trust store: {STORE}")
    print(f"P4 header: {P4_HEADER}")
    print(f"Cardputer header: {CARD_HEADER}")


if __name__ == "__main__":
    main()
