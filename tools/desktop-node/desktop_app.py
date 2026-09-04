#!/usr/bin/env python3
"""Local Reconclave web node and coordinator."""

from __future__ import annotations

import argparse
import concurrent.futures
import ipaddress
import json
import mimetypes
import os
import pathlib
import signal
import socket
import threading
import time
import urllib.parse
import uuid
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

from zeroconf import ServiceInfo, Zeroconf

from coordinator import Coordinator
from reconclave_node import ANNOUNCE_PATH, MESSAGE_PATH, PROTOCOL, Node, local_ip
from workspace_store import WorkspaceStore

WEB_ROOT = pathlib.Path(__file__).parent / "web" / "dist"
DEFAULT_TRUST_STORE = pathlib.Path(__file__).resolve().parents[2] / ".reconclave-provisioning" / "fleet.json"
DEFAULT_WORKSPACE_STORE = pathlib.Path(__file__).resolve().parents[2] / ".reconclave-data" / "workspace.json"
MAX_BODY_BYTES = 16 * 1024
MAX_INSPECTION_HOSTS = 16
MAX_INSPECTION_PORTS = 128


def inspect_hosts(local_address: str, body: dict) -> dict:
    if body.get("operator_authorised") is not True:
        raise PermissionError("explicit host inspection authorization acknowledgement is required")
    local = ipaddress.ip_address(local_address)
    network = ipaddress.ip_network(f"{local}/24", strict=False)
    raw_hosts = body.get("hosts", [])
    raw_ports = body.get("ports", [])
    if not isinstance(raw_hosts, list) or not 1 <= len(raw_hosts) <= MAX_INSPECTION_HOSTS:
        raise ValueError(f"hosts must contain 1-{MAX_INSPECTION_HOSTS} addresses")
    if not isinstance(raw_ports, list) or not 1 <= len(raw_ports) <= MAX_INSPECTION_PORTS:
        raise ValueError(f"ports must contain 1-{MAX_INSPECTION_PORTS} values")
    hosts = []
    for value in dict.fromkeys(str(item) for item in raw_hosts):
        address = ipaddress.ip_address(value)
        if address.version != 4 or address not in network or address in (network.network_address, network.broadcast_address):
            raise ValueError("every target must be a usable address on the attached /24")
        hosts.append(str(address))
    ports = []
    for value in dict.fromkeys(raw_ports):
        port = int(value)
        if not 1 <= port <= 65535:
            raise ValueError("ports must be between 1 and 65535")
        ports.append(port)

    def probe(pair: tuple[str, int]) -> tuple[str, int, bool]:
        host, port = pair
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as connection:
            connection.settimeout(0.35)
            return host, port, connection.connect_ex((host, port)) == 0

    results = {host: [] for host in hosts}
    pairs = [(host, port) for host in hosts for port in ports]
    with concurrent.futures.ThreadPoolExecutor(max_workers=min(64, len(pairs))) as pool:
        for host, port, opened in pool.map(probe, pairs):
            if opened:
                results[host].append(port)
    return {"hosts": [{"address": host, "open_ports": sorted(results[host]),
                        "checked_ports": len(ports)} for host in hosts],
            "ports": sorted(ports), "checked": len(pairs)}


def validate_scan_arguments(arguments: dict) -> None:
    """Reject broad or internally inconsistent assessment scopes before dispatch."""
    network = ipaddress.ip_network(str(arguments.get("network", "")), strict=True)
    if network.version != 4 or network.num_addresses > 256:
        raise ValueError("network must be an IPv4 /24 or smaller")
    first = ipaddress.ip_address(str(arguments.get("start_ip", "")))
    last = ipaddress.ip_address(str(arguments.get("end_ip", "")))
    if (first not in network or last not in network or first > last or
            first == network.network_address or last == network.broadcast_address):
        raise ValueError("scan range must contain usable addresses inside network")


def load_trust_keys(path: pathlib.Path | None, coordinator_id: str) -> dict[str, bytes]:
    if path is None or not path.is_file():
        return {}
    document = json.loads(path.read_text(encoding="utf-8"))
    keys = {}
    for link, value in document.get("links", {}).items():
        peers = link.split("|")
        if len(peers) != 2 or coordinator_id not in peers:
            continue
        peer_id = peers[1] if peers[0] == coordinator_id else peers[0]
        if not isinstance(value, str) or len(value) != 64:
            raise ValueError(f"invalid trust key for {peer_id}")
        keys[peer_id] = bytes.fromhex(value)
    return keys


class AppHandler(BaseHTTPRequestHandler):
    server: "AppServer"

    def local_client(self) -> bool:
        return self.client_address[0] in ("127.0.0.1", "::1")

    def send_bytes(self, status: int, body: bytes, content_type: str) -> None:
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store" if content_type == "application/json" else "public, max-age=3600")
        self.send_header("X-Content-Type-Options", "nosniff")
        self.send_header("X-Frame-Options", "DENY")
        self.end_headers()
        self.wfile.write(body)

    def send_json(self, status: int, body: dict) -> None:
        self.send_bytes(status, json.dumps(body, separators=(",", ":")).encode(), "application/json")

    def do_GET(self) -> None:
        path = urllib.parse.urlsplit(self.path).path
        if path == ANNOUNCE_PATH:
            self.send_json(200, self.server.node.announcement())
        elif path == "/api/state":
            if self.local_client():
                self.send_json(200, self.server.coordinator.state())
            else:
                self.send_json(403, {"error": "local_access_only"})
        elif path == "/api/workspace":
            if self.local_client():
                self.send_json(200, self.server.workspace.snapshot())
            else:
                self.send_json(403, {"error": "local_access_only"})
        elif path == "/api/events":
            if self.local_client():
                self.stream_events()
            else:
                self.send_json(403, {"error": "local_access_only"})
        elif path.startswith("/api/"):
            self.send_json(404, {"error": "not_found"})
        else:
            self.serve_web(path)

    def do_POST(self) -> None:
        path = urllib.parse.urlsplit(self.path).path
        if path.startswith("/api/") and not self.local_client():
            self.send_json(403, {"error": "local_access_only"})
            return
        try:
            length = int(self.headers.get("Content-Length", "0"))
            if length <= 0 or length > MAX_BODY_BYTES:
                raise ValueError("invalid body length")
            body = json.loads(self.rfile.read(length))
            if not isinstance(body, dict):
                raise ValueError("body must be an object")
            if path == MESSAGE_PATH:
                status, response = self.server.node.respond(body)
                self.send_json(status, response)
                return
            if path == "/api/projects":
                self.send_json(201, self.server.workspace.create_project(body))
                return
            if path == "/api/jobs":
                self.send_json(200, self.server.workspace.upsert_job(body))
                return
            if path == "/api/evidence":
                self.send_json(201, self.server.workspace.add_evidence(body))
                return
            if path == "/api/inspect":
                result = inspect_hosts(self.server.node.address, body)
                project_id = str(body.get("project_id", ""))
                job_id = f"inspect-{uuid.uuid4().hex[:12]}"
                if project_id:
                    self.server.workspace.upsert_job({
                        "id": job_id, "project_id": project_id,
                        "provider_id": self.server.node.node_id,
                        "capability": "net.tcp.inspect", "status": "complete",
                        "checked": result["checked"], "total": result["checked"],
                        "hosts": [item["address"] for item in result["hosts"]],
                        "scope": {"ports": result["ports"]},
                    })
                    self.server.workspace.add_evidence({
                        "id": f"{job_id}-tcp", "project_id": project_id,
                        "job_id": job_id, "kind": "tcp-services",
                        "title": f"TCP inspection · {len(result['hosts'])} host(s)",
                        "summary": f"{sum(len(item['open_ports']) for item in result['hosts'])} open ports observed",
                        "data": result,
                    })
                result["job_id"] = job_id
                result["captured_at_ms"] = int(time.time() * 1000)
                self.send_json(200, result)
                return
            parts = path.strip("/").split("/")
            if len(parts) == 4 and parts[:2] == ["api", "nodes"] and parts[3] == "invoke":
                capability = str(body.get("capability", ""))
                arguments = body.get("arguments", {})
                if not isinstance(arguments, dict):
                    raise ValueError("arguments must be an object")
                if capability == "net.discovery.scan":
                    if body.get("operator_authorised") is not True:
                        raise PermissionError("explicit scope authorization acknowledgement is required")
                    validate_scan_arguments(arguments)
                response = self.server.coordinator.invoke(
                    urllib.parse.unquote(parts[2]), capability, arguments)
                self.send_json(200, response)
                return
            self.send_json(404, {"error": "not_found"})
        except PermissionError as error:
            self.send_json(403, {"error": "trust_required", "message": str(error)})
        except KeyError as error:
            self.send_json(404, {"error": "node_unavailable", "message": str(error)})
        except (ValueError, json.JSONDecodeError) as error:
            self.send_json(400, {"error": "invalid_request", "message": str(error)})
        except ConnectionError as error:
            self.send_json(502, {"error": "node_request_failed", "message": str(error)})

    def stream_events(self) -> None:
        try:
            revision = int(self.headers.get("Last-Event-ID", "0"))
        except ValueError:
            revision = 0
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Cache-Control", "no-cache")
        self.send_header("Connection", "keep-alive")
        self.end_headers()
        try:
            while True:
                state = self.server.coordinator.wait_for_change(revision)
                revision = state["revision"]
                encoded = json.dumps(state, separators=(",", ":"))
                self.wfile.write(f"id: {revision}\nevent: state\ndata: {encoded}\n\n".encode())
                self.wfile.flush()
        except (BrokenPipeError, ConnectionResetError):
            return

    def serve_web(self, path: str) -> None:
        relative = path.lstrip("/") or "index.html"
        candidate = (WEB_ROOT / relative).resolve()
        if WEB_ROOT.resolve() not in candidate.parents or not candidate.is_file():
            candidate = WEB_ROOT / "index.html"
        if not candidate.is_file():
            self.send_json(503, {"error": "web_ui_not_built",
                                 "message": "Run npm install && npm run build in tools/desktop-node/web"})
            return
        mime = mimetypes.guess_type(candidate.name)[0] or "application/octet-stream"
        self.send_bytes(200, candidate.read_bytes(), mime)

    def log_message(self, fmt: str, *args: object) -> None:
        print(f"[{self.log_date_time_string()}] {fmt % args}")


class AppServer(ThreadingHTTPServer):
    daemon_threads = True

    def __init__(self, address: tuple[str, int], node: Node, coordinator: Coordinator,
                 workspace: WorkspaceStore) -> None:
        self.node = node
        self.coordinator = coordinator
        self.workspace = workspace
        super().__init__(address, AppHandler)


def main() -> None:
    parser = argparse.ArgumentParser(description="Run the Reconclave desktop web coordinator")
    parser.add_argument("--address", default=local_ip(), help="LAN IPv4 address to advertise")
    parser.add_argument("--port", type=int, default=8767)
    parser.add_argument("--name", default=socket.gethostname())
    parser.add_argument("--node-id", default=f"rc-desktop-{uuid.getnode():012x}")
    parser.add_argument("--mode", choices=("node", "coordinator", "both"), default="both")
    parser.add_argument("--enable-network-scan", action="store_true")
    parser.add_argument("--evidence-dir", default=None)
    parser.add_argument("--execution-key", default=os.environ.get("RECONCLAVE_EXECUTION_KEY"))
    parser.add_argument("--evidence-key", default=os.environ.get("RECONCLAVE_EVIDENCE_KEY"))
    parser.add_argument("--trust-store", type=pathlib.Path, default=DEFAULT_TRUST_STORE,
                        help="ignored per-link provisioning store")
    parser.add_argument("--workspace-store", type=pathlib.Path, default=DEFAULT_WORKSPACE_STORE,
                        help="local project/job/evidence index")
    args = parser.parse_args()
    if args.enable_network_scan and not args.execution_key:
        parser.error("an execution key is required when network scan is enabled")
    if args.evidence_dir and not args.evidence_key:
        parser.error("an evidence key is required when evidence storage is enabled")

    node_enabled = args.mode in ("node", "both")
    node = Node(args.node_id, args.name, args.address, args.port,
                node_enabled and args.enable_network_scan,
                args.evidence_dir if node_enabled else None,
                args.evidence_key, args.execution_key)
    node.roles = ["node"] + (["coordinator"] if args.mode in ("coordinator", "both") else [])
    zeroconf = Zeroconf()
    try:
        trust_keys = load_trust_keys(args.trust_store, args.node_id)
    except (OSError, ValueError, json.JSONDecodeError) as error:
        parser.error(f"could not load trust store: {error}")
    coordinator = Coordinator(node, zeroconf, args.execution_key, args.evidence_key,
                              trust_keys=trust_keys)
    if args.mode in ("coordinator", "both"):
        coordinator.start()
    try:
        workspace = WorkspaceStore(args.workspace_store)
    except (OSError, ValueError, json.JSONDecodeError) as error:
        parser.error(f"could not load workspace store: {error}")
    server = AppServer(("0.0.0.0", args.port), node, coordinator, workspace)
    service = ServiceInfo(
        "_reconclave._tcp.local.", f"{args.node_id}._reconclave._tcp.local.",
        addresses=[socket.inet_aton(args.address)], port=args.port,
        properties={"proto": PROTOCOL, "roles": ",".join(node.roles),
                    "device": "desktop-web", "path": ANNOUNCE_PATH},
        server=f"{args.node_id}.local.")
    zeroconf.register_service(service)

    def stop(_signum: int, _frame: object) -> None:
        threading.Thread(target=server.shutdown, daemon=True).start()

    signal.signal(signal.SIGINT, stop)
    signal.signal(signal.SIGTERM, stop)
    print(f"Reconclave desktop {args.mode} at http://127.0.0.1:{args.port}")
    print(f"Provisioned peer identities: {len(trust_keys)}")
    try:
        server.serve_forever(poll_interval=0.25)
    finally:
        coordinator.close()
        zeroconf.unregister_service(service)
        zeroconf.close()
        server.server_close()


if __name__ == "__main__":
    main()
