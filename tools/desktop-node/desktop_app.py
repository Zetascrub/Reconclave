#!/usr/bin/env python3
"""Local Reconclave web node and coordinator."""

from __future__ import annotations

import argparse
import ipaddress
import json
import mimetypes
import os
import pathlib
import signal
import socket
import threading
import urllib.parse
import uuid
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

from zeroconf import ServiceInfo, Zeroconf

from coordinator import Coordinator
from reconclave_node import ANNOUNCE_PATH, MESSAGE_PATH, PROTOCOL, Node, local_ip

WEB_ROOT = pathlib.Path(__file__).parent / "web" / "dist"
MAX_BODY_BYTES = 16 * 1024


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

    def __init__(self, address: tuple[str, int], node: Node, coordinator: Coordinator) -> None:
        self.node = node
        self.coordinator = coordinator
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
    coordinator = Coordinator(node, zeroconf, args.execution_key, args.evidence_key)
    if args.mode in ("coordinator", "both"):
        coordinator.start()
    server = AppServer(("0.0.0.0", args.port), node, coordinator)
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
    try:
        server.serve_forever(poll_interval=0.25)
    finally:
        coordinator.close()
        zeroconf.unregister_service(service)
        zeroconf.close()
        server.server_close()


if __name__ == "__main__":
    main()
