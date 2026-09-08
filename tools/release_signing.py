#!/usr/bin/env python3
"""Offline Ed25519 artifact signatures. Does not enable ESP Secure Boot."""
from __future__ import annotations
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import sys
from cryptography.exceptions import InvalidSignature
from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey, Ed25519PublicKey

SCHEMA = "reconclave-release/1"
MAX_MANIFEST = 16384


def exclusive_write(path: Path, data: bytes, mode: int = 0o600) -> None:
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, mode)
    with os.fdopen(fd, "wb") as stream:
        stream.write(data)
        stream.flush()
        os.fsync(stream.fileno())


def public_bytes(key: Ed25519PublicKey) -> bytes:
    return key.public_bytes(serialization.Encoding.PEM, serialization.PublicFormat.SubjectPublicKeyInfo)


def fingerprint(key: Ed25519PublicKey) -> str:
    raw = key.public_bytes(serialization.Encoding.Raw, serialization.PublicFormat.Raw)
    return hashlib.sha256(raw).hexdigest()


def private_key(path: Path) -> Ed25519PrivateKey:
    if path.is_symlink() or path.stat().st_mode & 0o077:
        raise ValueError("private key must be a regular, owner-only file (chmod 600)")
    key = serialization.load_pem_private_key(path.read_bytes(), password=None)
    if not isinstance(key, Ed25519PrivateKey):
        raise ValueError("an Ed25519 private key is required")
    return key


def init_key(private: Path, public: Path) -> str:
    private.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
    if private.exists():
        key = private_key(private)
    else:
        key = Ed25519PrivateKey.generate()
        exclusive_write(private, key.private_bytes(serialization.Encoding.PEM,
            serialization.PrivateFormat.PKCS8, serialization.NoEncryption()))
    encoded = public_bytes(key.public_key())
    public.parent.mkdir(parents=True, exist_ok=True)
    if public.exists():
        if public.read_bytes() != encoded:
            raise ValueError("existing public key differs; refusing to replace it")
    else:
        exclusive_write(public, encoded, 0o644)
    return fingerprint(key.public_key())


def digest(path: Path) -> tuple[int, str]:
    total = 0
    value = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            total += len(chunk)
            value.update(chunk)
    if not total:
        raise ValueError("artifact is empty")
    return total, value.hexdigest()


def canonical(value: dict) -> bytes:
    return json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=True).encode("ascii")


def sign(artifact: Path, private: Path, manifest: Path, version: str,
         target: str, commit: str, kind: str, dirty_source: bool = False) -> None:
    if not re.fullmatch(r"[A-Za-z0-9._+-]{1,64}", version):
        raise ValueError("invalid release version")
    if target not in ("source", "cardputer-adv", "poe-p4"):
        raise ValueError("unsupported target")
    if not re.fullmatch(r"[0-9a-f]{40}", commit):
        raise ValueError("full source commit SHA required")
    if kind not in ("source", "private-deployment-firmware") or (kind == "source") != (target == "source"):
        raise ValueError("firmware must be classified as private-deployment-firmware")
    if dirty_source and kind == "source":
        raise ValueError("public source releases must use committed sources")
    key = private_key(private)
    size, sha = digest(artifact)
    payload = {"schema": SCHEMA, "algorithm": "Ed25519", "key_id": fingerprint(key.public_key()),
        "artifact": artifact.name, "size": size, "sha256": sha, "version": version,
        "target": target, "source_commit": commit, "source_dirty": dirty_source, "kind": kind}
    encoded = {"manifest": payload, "signature": key.sign(canonical(payload)).hex()}
    exclusive_write(manifest, json.dumps(encoded, indent=2).encode() + b"\n")


def no_duplicates(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError("duplicate JSON key")
        result[key] = value
    return result


def verify(artifact: Path, public: Path, manifest: Path, target: str,
           version: str | None = None) -> dict:
    if manifest.stat().st_size > MAX_MANIFEST:
        raise ValueError("manifest too large")
    document = json.loads(manifest.read_text(), object_pairs_hook=no_duplicates)
    if not isinstance(document, dict) or set(document) != {"manifest", "signature"}:
        raise ValueError("invalid signature document")
    payload = document["manifest"]
    fields = {"schema", "algorithm", "key_id", "artifact", "size", "sha256", "version", "target", "source_commit", "source_dirty", "kind"}
    if not isinstance(payload, dict) or set(payload) != fields:
        raise ValueError("invalid manifest fields")
    key = serialization.load_pem_public_key(public.read_bytes())
    if not isinstance(key, Ed25519PublicKey):
        raise ValueError("an Ed25519 public key is required")
    key.verify(bytes.fromhex(document["signature"]), canonical(payload))
    if payload["schema"] != SCHEMA or payload["algorithm"] != "Ed25519" or payload["key_id"] != fingerprint(key):
        raise ValueError("signature identity/schema mismatch")
    if payload["target"] != target or (version is not None and payload["version"] != version):
        raise ValueError("wrong target or release version")
    if payload["kind"] not in ("source", "private-deployment-firmware") or (payload["kind"] == "source") != (target == "source"):
        raise ValueError("invalid artifact classification")
    if type(payload["source_dirty"]) is not bool or (payload["kind"] == "source" and payload["source_dirty"]):
        raise ValueError("invalid source state")
    if artifact.name != payload["artifact"]:
        raise ValueError("artifact filename mismatch")
    size, sha = digest(artifact)
    if type(payload["size"]) is not int or size != payload["size"] or sha != payload["sha256"]:
        raise ValueError("artifact hash/size mismatch")
    return payload


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    init = sub.add_parser("keygen")
    init.add_argument("--private-key", type=Path, required=True)
    init.add_argument("--public-key", type=Path, required=True)
    for name in ("sign", "verify"):
        p = sub.add_parser(name)
        p.add_argument("--artifact", type=Path, required=True)
        p.add_argument("--manifest", type=Path, required=True)
        p.add_argument("--target", choices=("source", "cardputer-adv", "poe-p4"), required=True)
        p.add_argument("--version", required=name == "sign")
        if name == "sign":
            p.add_argument("--private-key", type=Path, required=True)
            p.add_argument("--commit", required=True)
            p.add_argument("--dirty-source", action="store_true", help="mark a private development build with uncommitted changes")
            p.add_argument("--kind", choices=("source", "private-deployment-firmware"), required=True)
        else:
            p.add_argument("--public-key", type=Path, required=True)
    args = parser.parse_args()
    try:
        if args.command == "keygen":
            print("Public key SHA-256:", init_key(args.private_key, args.public_key))
        elif args.command == "sign":
            sign(args.artifact, args.private_key, args.manifest, args.version, args.target, args.commit, args.kind, args.dirty_source)
            print("Signed manifest:", args.manifest)
        else:
            value = verify(args.artifact, args.public_key, args.manifest, args.target, args.version)
            print("Verified:", value["target"], value["version"], value["kind"], "uncommitted-source" if value["source_dirty"] else "committed-source")
        return 0
    except (OSError, ValueError, TypeError, KeyError, InvalidSignature) as error:
        print("Release verification/signing failed:", str(error) or "invalid signature", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
