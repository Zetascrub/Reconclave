"""Discovery and request routing for the Reconclave desktop coordinator."""

from __future__ import annotations

import hashlib
import hmac
import json
import secrets
import threading
import time
import urllib.error
import urllib.request
from dataclasses import dataclass
from typing import Callable

from zeroconf import ServiceBrowser, ServiceInfo, ServiceListener, Zeroconf

from reconclave_node import ANNOUNCE_PATH, AUTH_TAG_BYTES, MESSAGE_PATH, PROTOCOL, Node

NODE_TTL_SECONDS = 45.0
REQUEST_TIMEOUT_SECONDS = 4.0
REFRESH_INTERVAL_SECONDS = 12.0


@dataclass
class Peer:
    device_id: str
    address: str
    port: int
    announcement: dict
    last_seen: float

    def public(self, now: float) -> dict:
        payload = self.announcement.get("payload", {})
        return {
            "device_id": self.device_id,
            "device_type": payload.get("device_type", "unknown"),
            "firmware": payload.get("firmware", "unknown"),
            "roles": payload.get("roles", []),
            "capabilities": payload.get("capabilities", []),
            "capability_descriptors": payload.get("capability_descriptors", []),
            "resources": payload.get("resources", {}),
            "status": payload.get("status", "degraded"),
            "address": self.address,
            "port": self.port,
            "age_seconds": round(max(0.0, now - self.last_seen), 1),
            "local": False,
        }


class Coordinator(ServiceListener):
    """Maintains a live mDNS roster and routes capability requests."""

    def __init__(self, node: Node, zeroconf: Zeroconf,
                 execution_key: str | None = None,
                 evidence_key: str | None = None,
                 trust_keys: dict[str, bytes] | None = None,
                 clock: Callable[[], float] = time.monotonic) -> None:
        self.node = node
        self.zeroconf = zeroconf
        self.clock = clock
        self.execution_key = hashlib.sha256(execution_key.encode()).digest() if execution_key else None
        self.evidence_key = hashlib.sha256(evidence_key.encode()).digest() if evidence_key else None
        self.trust_keys = trust_keys or {}
        self.peers: dict[str, Peer] = {}
        self.lock = threading.RLock()
        self.changed = threading.Condition(self.lock)
        self.revision = 1
        self.browser: ServiceBrowser | None = None
        self.stopping = threading.Event()
        self.refresh_thread: threading.Thread | None = None

    def start(self) -> None:
        self.browser = ServiceBrowser(self.zeroconf, "_reconclave._tcp.local.", self)
        self.refresh_thread = threading.Thread(target=self._refresh_loop,
                                               name="reconclave-refresh", daemon=True)
        self.refresh_thread.start()

    def close(self) -> None:
        self.stopping.set()
        if self.browser is not None:
            self.browser.cancel()
        if self.refresh_thread is not None:
            self.refresh_thread.join(timeout=1.0)

    def _refresh_loop(self) -> None:
        while not self.stopping.wait(REFRESH_INTERVAL_SECONDS):
            with self.lock:
                targets = [(peer.address, peer.port,
                            str(peer.announcement.get("_service_name", "")))
                           for peer in self.peers.values()]
            for address, port, service_name in targets:
                if self.stopping.is_set():
                    return
                self.refresh_peer(address, port, service_name)
            self.expire()

    def _touch(self) -> None:
        self.revision += 1
        self.changed.notify_all()

    def add_service(self, zeroconf: Zeroconf, service_type: str, name: str) -> None:
        self._resolve(service_type, name)

    def update_service(self, zeroconf: Zeroconf, service_type: str, name: str) -> None:
        self._resolve(service_type, name)

    def remove_service(self, zeroconf: Zeroconf, service_type: str, name: str) -> None:
        with self.changed:
            removed = [key for key, peer in self.peers.items()
                       if peer.announcement.get("_service_name") == name]
            for key in removed:
                del self.peers[key]
            if removed:
                self._touch()

    def _resolve(self, service_type: str, name: str) -> None:
        info = self.zeroconf.get_service_info(service_type, name, timeout=1500)
        if info is None:
            return
        addresses = info.parsed_addresses()
        if not addresses:
            return
        threading.Thread(target=self.refresh_peer,
                         args=(addresses[0], info.port, name), daemon=True).start()

    def refresh_peer(self, address: str, port: int, service_name: str = "") -> bool:
        try:
            with urllib.request.urlopen(
                    f"http://{address}:{port}{ANNOUNCE_PATH}",
                    timeout=REQUEST_TIMEOUT_SECONDS) as response:
                announcement = json.load(response)
            payload = announcement.get("payload", {})
            device_id = str(payload.get("device_id", ""))
            if (announcement.get("proto") != PROTOCOL or
                    announcement.get("type") != "announce" or
                    not device_id or device_id == self.node.node_id):
                return False
            announcement["_service_name"] = service_name
            with self.changed:
                self.peers[device_id] = Peer(
                    device_id, address, port, announcement, self.clock())
                self._touch()
            return True
        except (OSError, ValueError, urllib.error.URLError):
            return False

    def expire(self) -> bool:
        cutoff = self.clock() - NODE_TTL_SECONDS
        with self.changed:
            expired = [key for key, peer in self.peers.items() if peer.last_seen < cutoff]
            for key in expired:
                del self.peers[key]
            if expired:
                self._touch()
            return bool(expired)

    def state(self) -> dict:
        self.expire()
        now = self.clock()
        local = self.node.announcement()["payload"]
        local_node = {
            **local,
            "address": self.node.address,
            "port": self.node.port,
            "age_seconds": 0,
            "local": True,
        }
        with self.lock:
            nodes = [local_node] + [peer.public(now) for peer in self.peers.values()]
            nodes.sort(key=lambda item: (not item["local"], item["device_id"]))
            return {"revision": self.revision, "nodes": nodes,
                    "coordinator_id": self.node.node_id,
                    "updated_at_ms": int(time.time() * 1000)}

    def wait_for_change(self, revision: int, timeout: float = 15.0) -> dict:
        with self.changed:
            if self.revision == revision:
                self.changed.wait(timeout)
        return self.state()

    def invoke(self, device_id: str, capability: str, arguments: dict) -> dict:
        if device_id == self.node.node_id:
            target_address, target_port = self.node.address, self.node.port
            advertised = self.node.announcement()["payload"]
        else:
            with self.lock:
                peer = self.peers.get(device_id)
            if peer is None:
                raise KeyError("node is no longer available")
            target_address, target_port = peer.address, peer.port
            advertised = peer.announcement.get("payload", {})
        if capability not in advertised.get("capabilities", []):
            raise ValueError("capability is not advertised by this node")

        request_id = f"web-{secrets.token_hex(6)}"
        request = self.node.envelope("request", device_id)
        payload = {"request_id": request_id, "capability": capability,
                   "arguments": arguments}
        permission = "public"
        for descriptor in advertised.get("capability_descriptors", []):
            if descriptor.get("id") == capability:
                permission = descriptor.get("permission", "public")
                break
        if permission == "trusted":
            key = (self.evidence_key if capability == "storage.evidence.write" else
                   self.trust_keys.get(device_id, self.execution_key))
            if key is None:
                raise PermissionError("the required trust-domain key is not configured")
            nonce = secrets.token_hex(8)
            boot_nonce = str(advertised.get("security", {}).get("boot_nonce", ""))
            fields = [self.node.node_id, device_id, request_id, capability]
            if boot_nonce:
                fields.append(boot_nonce)
            fields.append(nonce)
            canonical = "|".join(fields).encode()
            payload["auth"] = {
                "nonce": nonce,
                "tag": hmac.new(key, canonical, hashlib.sha256).digest()[:AUTH_TAG_BYTES].hex(),
            }
        request["payload"] = payload
        encoded = json.dumps(request, separators=(",", ":")).encode()
        http_request = urllib.request.Request(
            f"http://{target_address}:{target_port}{MESSAGE_PATH}", data=encoded,
            headers={"Content-Type": "application/json"}, method="POST")
        try:
            with urllib.request.urlopen(http_request, timeout=REQUEST_TIMEOUT_SECONDS) as response:
                result = json.load(response)
        except urllib.error.HTTPError as error:
            raise ConnectionError(f"node returned HTTP {error.code}") from error
        except (OSError, ValueError, urllib.error.URLError) as error:
            raise ConnectionError("node request failed") from error
        if result.get("payload", {}).get("request_id") != request_id:
            raise ConnectionError("node returned a mismatched response")
        if permission == "trusted":
            response_payload = result.get("payload", {})
            response_auth = response_payload.get("auth", {})
            response_nonce = response_auth.get("nonce")
            response_tag = response_auth.get("tag")
            status = response_payload.get("status", "")
            if response_nonce != nonce or not isinstance(response_tag, str):
                raise ConnectionError("node returned an unauthenticated response")
            response_canonical = "|".join([
                device_id, self.node.node_id, request_id, status, boot_nonce, nonce,
            ]).encode()
            expected_tag = hmac.new(key, response_canonical, hashlib.sha256).digest()[:AUTH_TAG_BYTES].hex()
            if not hmac.compare_digest(response_tag, expected_tag):
                raise ConnectionError("node returned an invalid response authentication tag")
        if device_id != self.node.node_id:
            with self.changed:
                current = self.peers.get(device_id)
                if current is not None:
                    current.last_seen = self.clock()
        return result
