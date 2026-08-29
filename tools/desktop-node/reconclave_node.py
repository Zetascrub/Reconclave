#!/usr/bin/env python3
"""A small, read-only Reconclave node for a desktop or laptop."""

from __future__ import annotations

import argparse
import collections
import datetime
import errno
import hashlib
import hmac
import ipaddress
import json
import os
import platform
import shutil
import signal
import socket
import threading
import time
import uuid
from concurrent.futures import ThreadPoolExecutor, as_completed
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

from zeroconf import ServiceInfo, Zeroconf

PROTOCOL = "reconclave/1"
FIRMWARE = "0.1.0"
ANNOUNCE_PATH = "/reconclave/v1/announce"
MESSAGE_PATH = "/reconclave/v1/message"
MAX_EVIDENCE_RECORD_BYTES = 16 * 1024
EVIDENCE_REQUIRED_FIELDS = ("job_id", "source_node", "target", "timestamp_ms", "observation")
# These two capabilities can write persistent state or stop someone else's job, so
# unlike the read-only capabilities they require a signed, replay-checked request.
AUTH_REQUIRED_CAPABILITIES = {"storage.evidence.write", "coordination.job.cancel"}
AUTH_TAG_BYTES = 16
RECENT_NONCES_PER_SOURCE = 16


class CapabilityError(Exception):
    """Raised by a capability handler for a request that reaches execution but must
    be refused (rejected) or that fails while running (error)."""

    def __init__(self, code: str, message: str, status: str = "rejected") -> None:
        super().__init__(message)
        self.code = code
        self.message = message
        self.status = status


def local_ip() -> str:
    probe = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        probe.connect(("192.0.2.1", 9))
        return probe.getsockname()[0]
    except OSError:
        return socket.gethostbyname(socket.gethostname())
    finally:
        probe.close()


class Node:
    def __init__(self, node_id: str, name: str, address: str, port: int,
                 enable_network_scan: bool = False, evidence_dir: str | None = None,
                 evidence_key: str | None = None) -> None:
        self.node_id = node_id
        self.name = name
        self.address = address
        self.port = port
        self.started = time.monotonic()
        self.sequence = 0
        self.lock = threading.Lock()
        self.scan_lock = threading.Lock()
        self.scan_cancelled = threading.Event()
        self.scan_state = {
            "job_status": "idle", "checked": 0, "total": 0, "hosts": [], "error": "",
            "recurring": False, "run_count": 0,
        }
        self.evidence_dir = evidence_dir
        self.evidence_lock = threading.Lock()
        # The passphrase itself is never stored, only its digest - mirrors how the
        # Cardputer's Grove-paired peer key is kept.
        self.auth_key = hashlib.sha256(evidence_key.encode()).digest() if evidence_key else None
        self.nonce_lock = threading.Lock()
        self.recent_nonces: dict[str, collections.deque] = {}
        # Announcements are generated from this dispatcher. Adding a handler
        # automatically advertises it; removing one removes the claim.
        self.capability_handlers = {
            "system.info": self.system_info,
            "desktop.resources": self.desktop_resources,
        }
        if enable_network_scan:
            self.capability_handlers["net.discovery.scan"] = self.start_network_scan
            self.capability_handlers["coordination.job.status"] = self.network_scan_status
            if self.auth_key is not None:
                self.capability_handlers["coordination.job.cancel"] = self.cancel_network_scan
        if evidence_dir is not None and self.auth_key is not None:
            self.capability_handlers["storage.evidence.write"] = self.write_evidence

    def verify_auth(self, source_node: str, destination_node: str, request_id: str,
                     capability: str, auth: object) -> None:
        """Raises CapabilityError unless `auth` is a valid, fresh signature over this
        exact request. Only called for capabilities in AUTH_REQUIRED_CAPABILITIES,
        which are only ever registered once self.auth_key is set."""
        if not isinstance(auth, dict):
            raise CapabilityError("UNAUTHENTICATED", "request is not signed")
        nonce = str(auth.get("nonce", ""))
        tag_hex = str(auth.get("tag", ""))
        if not nonce or not tag_hex:
            raise CapabilityError("UNAUTHENTICATED", "request is not signed")
        canonical = f"{source_node}|{destination_node}|{request_id}|{capability}|{nonce}".encode()
        expected = hmac.new(self.auth_key, canonical, hashlib.sha256).digest()[:AUTH_TAG_BYTES]
        try:
            supplied = bytes.fromhex(tag_hex)
        except ValueError:
            raise CapabilityError("UNAUTHENTICATED", "malformed signature") from None
        if not hmac.compare_digest(expected, supplied):
            raise CapabilityError("UNAUTHENTICATED", "invalid signature")
        with self.nonce_lock:
            seen = self.recent_nonces.setdefault(
                source_node, collections.deque(maxlen=RECENT_NONCES_PER_SOURCE))
            if nonce in seen:
                raise CapabilityError("UNAUTHENTICATED", "replayed request")
            seen.append(nonce)

    def system_info(self, _arguments: dict) -> dict:
        return {
            "device_type": "desktop-node",
            "firmware": FIRMWARE,
            "name": self.name,
            "platform": platform.system(),
            "platform_release": platform.release(),
            "hostname": socket.gethostname(),
            "uptime_ms": int((time.monotonic() - self.started) * 1000),
            "ip": self.address,
        }

    def desktop_resources(self, _arguments: dict) -> dict:
        disk = shutil.disk_usage("/")
        result = {
            "load_average": list(os.getloadavg()) if hasattr(os, "getloadavg") else [],
            "cpu_count": os.cpu_count() or 0,
            "disk_total_bytes": disk.total,
            "disk_free_bytes": disk.free,
        }
        if hasattr(os, "sysconf"):
            try:
                result["memory_total_bytes"] = os.sysconf("SC_PAGE_SIZE") * os.sysconf("SC_PHYS_PAGES")
                result["memory_available_bytes"] = os.sysconf("SC_PAGE_SIZE") * os.sysconf("SC_AVPHYS_PAGES")
            except (ValueError, OSError):
                pass
        return result

    def network_scan_status(self, _arguments: dict) -> dict:
        with self.scan_lock:
            return self.network_scan_status_unlocked()

    def cancel_network_scan(self, _arguments: dict) -> dict:
        # Idempotent: cancelling with nothing running is still a successful no-op.
        self.scan_cancelled.set()
        with self.scan_lock:
            if self.scan_state["job_status"] == "running":
                self.scan_state["recurring"] = False
            return self.network_scan_status_unlocked()

    @staticmethod
    def _parse_schedule(arguments: dict) -> int | None:
        schedule = arguments.get("schedule")
        if not schedule:
            return None
        interval_ms = int(schedule.get("interval_ms", 0))
        if interval_ms <= 0:
            raise ValueError("schedule.interval_ms must be a positive integer")
        return interval_ms

    def start_network_scan(self, arguments: dict) -> dict:
        with self.scan_lock:
            if self.scan_state["job_status"] == "running":
                return self.network_scan_status_unlocked()
            try:
                network = ipaddress.ip_network(arguments.get("network", f"{self.address}/24"), strict=False)
                if network.version != 4 or network.num_addresses > 256:
                    raise ValueError("network must be an IPv4 /24 or smaller")
                first = ipaddress.ip_address(arguments.get("start_ip", str(network.network_address + 1)))
                last = ipaddress.ip_address(arguments.get("end_ip", str(network.broadcast_address - 1)))
                if (first not in network or last not in network or first > last or
                        first == network.network_address or last == network.broadcast_address):
                    raise ValueError("scan range must contain usable addresses inside network")
                interval_ms = self._parse_schedule(arguments)
            except ValueError as error:
                self.scan_state = {
                    "job_status": "failed", "checked": 0, "total": 0, "hosts": [], "error": str(error),
                    "recurring": False, "run_count": 0,
                }
                return self.network_scan_status_unlocked()
            hosts = [str(ipaddress.ip_address(value)) for value in range(int(first), int(last) + 1)
                     if str(ipaddress.ip_address(value)) != self.address]
            self.scan_cancelled.clear()
            self.scan_state = {
                "job_status": "running", "checked": 0, "total": len(hosts), "hosts": [], "error": "",
                "recurring": interval_ms is not None, "run_count": 0,
            }
        threading.Thread(target=self._scan_network, args=(hosts, interval_ms), daemon=True).start()
        return self.network_scan_status({})

    def network_scan_status_unlocked(self) -> dict:
        return {
            "job_status": self.scan_state["job_status"],
            "checked": self.scan_state["checked"],
            "total": self.scan_state["total"],
            "hosts": list(self.scan_state["hosts"]),
            "error": self.scan_state["error"],
            "recurring": self.scan_state["recurring"],
            "run_count": self.scan_state["run_count"],
        }

    @staticmethod
    def _host_responds(address: str) -> bool:
        # A successful connection or an explicit refusal both prove a host is present.
        for port in (80, 443, 22, 445, 3389):
            sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            sock.settimeout(0.12)
            try:
                result = sock.connect_ex((address, port))
                if result in (0, errno.ECONNREFUSED):
                    return True
            finally:
                sock.close()
        return False

    def _scan_network(self, hosts: list[str], interval_ms: int | None) -> None:
        while True:
            try:
                with ThreadPoolExecutor(max_workers=32, thread_name_prefix="recon-scan") as pool:
                    futures = {pool.submit(self._host_responds, host): host for host in hosts}
                    for future in as_completed(futures):
                        responsive = future.result()
                        with self.scan_lock:
                            self.scan_state["checked"] += 1
                            if responsive:
                                self.scan_state["hosts"].append(futures[future])
                with self.scan_lock:
                    self.scan_state["hosts"].sort(key=ipaddress.ip_address)
                    self.scan_state["run_count"] += 1
                    still_recurring = self.scan_state["recurring"] and not self.scan_cancelled.is_set()
                    self.scan_state["job_status"] = "running" if still_recurring else "complete"
            except Exception as error:  # Preserve job visibility instead of losing the worker silently.
                with self.scan_lock:
                    self.scan_state["job_status"] = "failed"
                    self.scan_state["error"] = str(error)[:160]
                return
            if not still_recurring or interval_ms is None:
                return
            # after_completion semantics: the interval is measured from this run's
            # end, so a slow pass never overlaps the next one.
            if self.scan_cancelled.wait(interval_ms / 1000):
                with self.scan_lock:
                    self.scan_state["job_status"] = "complete"
                    self.scan_state["recurring"] = False
                return
            with self.scan_lock:
                self.scan_state["checked"] = 0
                self.scan_state["hosts"] = []

    def write_evidence(self, arguments: dict) -> dict:
        record = arguments.get("evidence")
        if not isinstance(record, dict) or any(field not in record for field in EVIDENCE_REQUIRED_FIELDS):
            raise CapabilityError("INVALID_REQUEST", "evidence record missing a required field")
        encoded = json.dumps(record, separators=(",", ":"))
        if len(encoded.encode()) > MAX_EVIDENCE_RECORD_BYTES:
            raise CapabilityError("INVALID_REQUEST", "evidence record exceeds size limit")
        assert self.evidence_dir is not None  # capability is only registered when set
        try:
            os.makedirs(self.evidence_dir, exist_ok=True)
            day = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%d")
            path = os.path.join(self.evidence_dir, f"evidence-{day}.jsonl")
            with self.evidence_lock, open(path, "a", encoding="utf-8") as handle:
                handle.write(encoded + "\n")
        except OSError as error:
            raise CapabilityError("STORAGE_UNAVAILABLE", str(error)[:160], status="error") from error
        return {"stored": True}

    def envelope(self, message_type: str, destination: str | None = None) -> dict:
        with self.lock:
            self.sequence += 1
            sequence = self.sequence
        message = {
            "proto": PROTOCOL,
            "type": message_type,
            "message_id": f"{self.node_id}-{sequence}",
            "source_node": self.node_id,
            "timestamp_ms": int(time.time() * 1000),
            "sequence": sequence,
            "payload": {},
        }
        if destination:
            message["destination_node"] = destination
        return message

    def announcement(self) -> dict:
        message = self.envelope("announce")
        message["payload"] = {
            "device_id": self.node_id,
            "device_type": "desktop-node",
            "firmware": FIRMWARE,
            "roles": ["node"],
            "capabilities": sorted(self.capability_handlers),
            "status": "ready",
        }
        return message

    def respond(self, request: object) -> tuple[int, dict]:
        if not isinstance(request, dict):
            response = self.envelope("response", "unknown")
            response["payload"] = {
                "request_id": "invalid-request",
                "status": "rejected",
                "error": {"code": "INVALID_REQUEST", "message": "Request must be an object"},
            }
            return 200, response
        source = str(request.get("source_node", "unknown"))
        payload = request.get("payload", {})
        if not isinstance(payload, dict):
            payload = {}
        request_id = str(payload.get("request_id", "invalid-request"))
        response = self.envelope("response", source)
        valid = (
            request.get("proto") == PROTOCOL
            and request.get("type") == "request"
            and request.get("destination_node") == self.node_id
        )
        if not valid:
            response["payload"] = {
                "request_id": request_id,
                "status": "rejected",
                "error": {"code": "INVALID_REQUEST", "message": "Malformed or misdirected request"},
            }
        elif payload.get("capability") not in self.capability_handlers:
            response["payload"] = {
                "request_id": request_id,
                "status": "rejected",
                "error": {"code": "CAPABILITY_UNAVAILABLE", "message": "Capability is not available"},
            }
        else:
            capability = str(payload["capability"])
            try:
                arguments = payload.get("arguments", {})
                if not isinstance(arguments, dict):
                    raise CapabilityError("INVALID_REQUEST", "arguments must be an object")
                if capability in AUTH_REQUIRED_CAPABILITIES:
                    destination = str(request.get("destination_node", ""))
                    self.verify_auth(source, destination, request_id, capability, payload.get("auth"))
                result = self.capability_handlers[capability](arguments)
                response["payload"] = {"request_id": request_id, "status": "ok", "result": result}
            except CapabilityError as error:
                response["payload"] = {
                    "request_id": request_id,
                    "status": error.status,
                    "error": {"code": error.code, "message": error.message},
                }
        return 200, response


class Handler(BaseHTTPRequestHandler):
    server: "NodeServer"

    def send_json(self, status: int, body: dict) -> None:
        encoded = json.dumps(body, separators=(",", ":")).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(encoded)))
        self.end_headers()
        self.wfile.write(encoded)

    def do_GET(self) -> None:
        if self.path == ANNOUNCE_PATH:
            self.send_json(200, self.server.node.announcement())
        else:
            self.send_json(404, {"error": "not_found"})

    def do_POST(self) -> None:
        if self.path != MESSAGE_PATH:
            self.send_json(404, {"error": "not_found"})
            return
        try:
            length = int(self.headers.get("Content-Length", "0"))
            if length <= 0 or length > 16384:
                raise ValueError("invalid body length")
            request = json.loads(self.rfile.read(length))
            status, response = self.server.node.respond(request)
            self.send_json(status, response)
        except (ValueError, json.JSONDecodeError):
            self.send_json(400, {"error": "invalid_json"})

    def log_message(self, fmt: str, *args: object) -> None:
        print(f"[{self.log_date_time_string()}] {fmt % args}")


class NodeServer(ThreadingHTTPServer):
    daemon_threads = True

    def __init__(self, address: tuple[str, int], node: Node) -> None:
        self.node = node
        super().__init__(address, Handler)


def main() -> None:
    parser = argparse.ArgumentParser(description="Run a read-only Reconclave desktop node")
    parser.add_argument("--address", default=local_ip(), help="LAN IPv4 address to advertise")
    parser.add_argument("--port", type=int, default=8767)
    parser.add_argument("--name", default=socket.gethostname())
    parser.add_argument("--node-id", default=f"rc-desktop-{uuid.getnode():012x}")
    parser.add_argument("--enable-network-scan", action="store_true",
                        help="advertise and enable bounded local /24 discovery")
    parser.add_argument("--evidence-dir", default=None,
                        help="advertise storage.evidence.write and append records under this directory")
    parser.add_argument("--evidence-key", default=None,
                        help="shared passphrase required to sign storage.evidence.write and "
                             "coordination.job.cancel requests; must match the coordinator's key. "
                             "Neither capability is advertised without it")
    args = parser.parse_args()

    node = Node(args.node_id, args.name, args.address, args.port, args.enable_network_scan,
                args.evidence_dir, args.evidence_key)
    server = NodeServer(("0.0.0.0", args.port), node)
    service = ServiceInfo(
        "_reconclave._tcp.local.",
        f"{args.node_id}._reconclave._tcp.local.",
        addresses=[socket.inet_aton(args.address)],
        port=args.port,
        properties={"proto": PROTOCOL, "roles": "node", "device": "desktop-node", "path": ANNOUNCE_PATH},
        server=f"{args.node_id}.local.",
    )
    zeroconf = Zeroconf()
    zeroconf.register_service(service)
    stopping = threading.Event()

    def stop(_signum: int, _frame: object) -> None:
        stopping.set()
        threading.Thread(target=server.shutdown, daemon=True).start()

    signal.signal(signal.SIGINT, stop)
    signal.signal(signal.SIGTERM, stop)
    print(f"Reconclave desktop node {args.node_id}")
    print(f"Advertising http://{args.address}:{args.port}{ANNOUNCE_PATH}")
    try:
        server.serve_forever(poll_interval=0.25)
    finally:
        zeroconf.unregister_service(service)
        zeroconf.close()
        server.server_close()
        print("Node stopped")


if __name__ == "__main__":
    main()
