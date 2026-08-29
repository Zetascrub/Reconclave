#!/usr/bin/env python3
"""Dependency-free structural checks for Reconclave v1 test vectors."""

from __future__ import annotations

import argparse
import copy
import json
import re
import sys
from pathlib import Path
from typing import Any

IDENTIFIER = re.compile(r"^[A-Za-z0-9_.-]{1,64}$")
CAPABILITY = re.compile(r"^[a-z0-9_-]+(?:\.[a-z0-9_-]+)+$")
MESSAGE_TYPES = {"announce", "request", "response", "event", "stream"}
NODE_STATUSES = {"ready", "busy", "degraded"}
RESPONSE_STATUSES = {"accepted", "ok", "rejected", "error"}
EVENT_KINDS = {"progress", "evidence", "complete", "failed", "cancelled"}
MAX_PAYLOAD_BYTES = 16 * 1024


class InvalidMessage(ValueError):
    pass


def require(condition: bool, reason: str) -> None:
    if not condition:
        raise InvalidMessage(reason)


def identifier(value: Any, field: str) -> None:
    require(isinstance(value, str) and IDENTIFIER.fullmatch(value) is not None,
            f"{field} is not a portable identifier")


def exact_keys(value: dict[str, Any], required: set[str], optional: set[str] = set()) -> None:
    missing = required - value.keys()
    unknown = value.keys() - required - optional
    require(not missing, f"missing fields: {sorted(missing)}")
    require(not unknown, f"unknown fields: {sorted(unknown)}")


def validate_payload(message_type: str, payload: dict[str, Any], source: str) -> None:
    if message_type == "announce":
        exact_keys(payload, {"device_id", "device_type", "firmware", "roles", "capabilities", "status"},
                   {"security"})
        identifier(payload["device_id"], "device_id")
        identifier(payload["device_type"], "device_type")
        require(payload["device_id"] == source, "announcement identity differs from source_node")
        require(isinstance(payload["firmware"], str) and 0 < len(payload["firmware"]) <= 32,
                "invalid firmware")
        roles = payload["roles"]
        require(isinstance(roles, list) and 0 < len(roles) <= 4,
                "invalid roles list")
        require(all(role in {"node", "coordinator", "analysis"} for role in roles),
                "invalid role")
        require("node" in roles and len(roles) == len(set(roles)),
                "node role is required and roles must be unique")
        capabilities = payload["capabilities"]
        require(isinstance(capabilities, list) and len(capabilities) <= 64,
                "invalid capabilities list")
        require(all(isinstance(item, str) and CAPABILITY.fullmatch(item) for item in capabilities),
                "invalid capability")
        require(len(capabilities) == len(set(capabilities)), "duplicate capability")
        require(payload["status"] in NODE_STATUSES, "invalid node status")
    elif message_type == "request":
        exact_keys(payload, {"request_id", "capability", "arguments"}, {"auth"})
        identifier(payload["request_id"], "request_id")
        require(isinstance(payload["capability"], str) and CAPABILITY.fullmatch(payload["capability"]),
                "invalid capability")
        require(isinstance(payload["arguments"], dict), "arguments must be an object")
    elif message_type == "response":
        exact_keys(payload, {"request_id", "status"}, {"job_id", "result", "error", "auth"})
        identifier(payload["request_id"], "request_id")
        require(payload["status"] in RESPONSE_STATUSES, "invalid response status")
        if payload["status"] == "accepted":
            require("job_id" in payload, "accepted response requires job_id")
            identifier(payload["job_id"], "job_id")
        if payload["status"] in {"rejected", "error"}:
            require(isinstance(payload.get("error"), dict), "failure response requires error")
    elif message_type == "event":
        exact_keys(payload, {"job_id", "kind", "data"}, {"progress_percent"})
        identifier(payload["job_id"], "job_id")
        require(payload["kind"] in EVENT_KINDS, "invalid event kind")
        progress = payload.get("progress_percent", 0)
        require(isinstance(progress, int) and not isinstance(progress, bool) and 0 <= progress <= 100,
                "invalid progress")
        require(isinstance(payload["data"], dict), "event data must be an object")
    else:
        exact_keys(payload, {"stream_id", "chunk_index", "final", "encoding", "data"})
        identifier(payload["stream_id"], "stream_id")
        require(isinstance(payload["chunk_index"], int) and not isinstance(payload["chunk_index"], bool)
                and payload["chunk_index"] >= 0, "invalid chunk index")
        require(isinstance(payload["final"], bool), "final must be boolean")
        require(payload["encoding"] == "base64", "unsupported stream encoding")
        require(isinstance(payload["data"], str), "stream data must be a string")


def validate_message(message: Any) -> None:
    require(isinstance(message, dict), "message must be an object")
    exact_keys(message,
               {"proto", "type", "message_id", "source_node", "timestamp_ms", "sequence", "payload"},
               {"destination_node"})
    require(message["proto"] == "reconclave/1", "unsupported protocol")
    require(message["type"] in MESSAGE_TYPES, "unknown message type")
    identifier(message["message_id"], "message_id")
    identifier(message["source_node"], "source_node")
    destination = message.get("destination_node")
    if message["type"] == "announce":
        require(destination is None, "announcement must be broadcast")
    else:
        identifier(destination, "destination_node")
    for field in ("timestamp_ms", "sequence"):
        value = message[field]
        require(isinstance(value, int) and not isinstance(value, bool) and value > 0,
                f"{field} must be a positive integer")
    require(isinstance(message["payload"], dict), "payload must be an object")
    encoded_size = len(json.dumps(message["payload"], separators=(",", ":")).encode("utf-8"))
    require(encoded_size <= MAX_PAYLOAD_BYTES, "payload exceeds size limit")
    validate_payload(message["type"], message["payload"], message["source_node"])


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("paths", nargs="+", type=Path)
    args = parser.parse_args()
    messages: list[dict[str, Any]] = []
    failed = False
    for path in args.paths:
        try:
            message = json.loads(path.read_text(encoding="utf-8"))
            validate_message(message)
            messages.append(message)
            print(f"valid: {path}")
        except (OSError, json.JSONDecodeError, InvalidMessage) as error:
            failed = True
            print(f"invalid: {path}: {error}", file=sys.stderr)

    if messages:
        invalid = copy.deepcopy(messages[0])
        invalid["proto"] = "reconclave/99"
        try:
            validate_message(invalid)
            print("internal negative check unexpectedly passed", file=sys.stderr)
            failed = True
        except InvalidMessage:
            pass
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
