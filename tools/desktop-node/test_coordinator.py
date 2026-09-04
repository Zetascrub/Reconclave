from __future__ import annotations

import importlib.util
import pathlib
import sys
import types
import unittest
from unittest import mock


fake_zeroconf = sys.modules.get("zeroconf", types.ModuleType("zeroconf"))
fake_zeroconf.ServiceBrowser = mock.MagicMock
fake_zeroconf.ServiceInfo = mock.MagicMock
fake_zeroconf.ServiceListener = object
fake_zeroconf.Zeroconf = mock.MagicMock
sys.modules["zeroconf"] = fake_zeroconf

HERE = pathlib.Path(__file__).parent
node_spec = importlib.util.spec_from_file_location("reconclave_node", HERE / "reconclave_node.py")
node_module = importlib.util.module_from_spec(node_spec)
assert node_spec.loader is not None
node_spec.loader.exec_module(node_module)
sys.modules["reconclave_node"] = node_module
coordinator_spec = importlib.util.spec_from_file_location("coordinator", HERE / "coordinator.py")
coordinator_module = importlib.util.module_from_spec(coordinator_spec)
assert coordinator_spec.loader is not None
sys.modules["coordinator"] = coordinator_module
coordinator_spec.loader.exec_module(coordinator_module)
desktop_spec = importlib.util.spec_from_file_location("desktop_app", HERE / "desktop_app.py")
desktop_module = importlib.util.module_from_spec(desktop_spec)
assert desktop_spec.loader is not None
desktop_spec.loader.exec_module(desktop_module)


class CoordinatorTests(unittest.TestCase):
    def setUp(self):
        self.now = 100.0
        self.node = node_module.Node("rc-local", "Local", "127.0.0.1", 8767)
        self.node.roles = ["node", "coordinator"]
        self.coordinator = coordinator_module.Coordinator(
            self.node, mock.MagicMock(), execution_key="test execution",
            clock=lambda: self.now)

    def announcement(self, device_id="rc-peer", capabilities=None):
        return {
            "proto": "reconclave/1", "type": "announce",
            "payload": {
                "device_id": device_id, "device_type": "test-node", "firmware": "0.1",
                "roles": ["node"], "capabilities": capabilities or ["system.info"],
                "capability_descriptors": [], "resources": {}, "status": "ready",
            },
        }

    def test_refresh_adds_peer_and_expiry_removes_it(self):
        response = mock.MagicMock()
        response.__enter__.return_value = response
        response.__exit__.return_value = False
        with mock.patch.object(coordinator_module.urllib.request, "urlopen", return_value=response), \
             mock.patch.object(coordinator_module.json, "load", return_value=self.announcement()):
            self.assertTrue(self.coordinator.refresh_peer("192.0.2.8", 8767, "peer.local"))
        state = self.coordinator.state()
        self.assertEqual([item["device_id"] for item in state["nodes"]], ["rc-local", "rc-peer"])
        self.now += coordinator_module.NODE_TTL_SECONDS + 1
        self.assertEqual([item["device_id"] for item in self.coordinator.state()["nodes"]], ["rc-local"])

    def test_invoke_rejects_unadvertised_capability_without_network_request(self):
        peer = coordinator_module.Peer("rc-peer", "192.0.2.8", 8767,
                                       self.announcement(), self.now)
        self.coordinator.peers[peer.device_id] = peer
        with mock.patch.object(coordinator_module.urllib.request, "urlopen") as opener:
            with self.assertRaisesRegex(ValueError, "not advertised"):
                self.coordinator.invoke("rc-peer", "net.discovery.scan", {})
            opener.assert_not_called()

    def test_trusted_invoke_is_signed_with_execution_domain(self):
        announcement = self.announcement(capabilities=["net.discovery.scan"])
        announcement["payload"]["capability_descriptors"] = [{
            "id": "net.discovery.scan", "version": 1, "permission": "trusted",
            "features": [], "limits": {"weight": 1, "max_concurrency": 1},
        }]
        self.coordinator.peers["rc-peer"] = coordinator_module.Peer(
            "rc-peer", "192.0.2.8", 8767, announcement, self.now)
        response = mock.MagicMock()
        response.__enter__.return_value = response
        response.__exit__.return_value = False

        def response_document(stream):
            sent = stream
            if hasattr(stream, "full_url"):
                sent = stream
            request_body = sent.data
            request = coordinator_module.json.loads(request_body)
            request_id = request["payload"]["request_id"]
            self.assertIn("auth", request["payload"])
            return {"payload": {"request_id": request_id, "status": "ok", "result": {}}}

        with mock.patch.object(coordinator_module.urllib.request, "urlopen", return_value=response) as opener, \
             mock.patch.object(coordinator_module.json, "load") as loader:
            loader.side_effect = lambda _response: response_document(opener.call_args.args[0])
            result = self.coordinator.invoke("rc-peer", "net.discovery.scan", {"network": "192.0.2.0/24"})
        self.assertEqual(result["payload"]["status"], "ok")

    def test_p4_style_request_signature_includes_boot_nonce(self):
        announcement = self.announcement(capabilities=["net.discovery.scan"])
        announcement["payload"]["security"] = {"boot_nonce": "abc123", "mode": "hmac-sha256-128"}
        announcement["payload"]["capability_descriptors"] = [{
            "id": "net.discovery.scan", "version": 1, "permission": "trusted",
        }]
        self.coordinator.peers["rc-peer"] = coordinator_module.Peer(
            "rc-peer", "192.0.2.8", 8767, announcement, self.now)
        response = mock.MagicMock()
        response.__enter__.return_value = response
        response.__exit__.return_value = False
        captured = {}

        def load_response(_response):
            request = coordinator_module.json.loads(captured["request"].data)
            payload = request["payload"]
            canonical = (f"rc-local|rc-peer|{payload['request_id']}|net.discovery.scan|"
                         f"abc123|{payload['auth']['nonce']}").encode()
            expected = coordinator_module.hmac.new(
                self.coordinator.execution_key, canonical,
                coordinator_module.hashlib.sha256).digest()[:16].hex()
            self.assertEqual(payload["auth"]["tag"], expected)
            return {"payload": {"request_id": payload["request_id"], "status": "ok", "result": {}}}

        def open_request(request, timeout):
            captured["request"] = request
            return response

        with mock.patch.object(coordinator_module.urllib.request, "urlopen", side_effect=open_request), \
             mock.patch.object(coordinator_module.json, "load", side_effect=load_response):
            self.coordinator.invoke("rc-peer", "net.discovery.scan", {})

    def test_scan_scope_validation_is_bounded_and_consistent(self):
        desktop_module.validate_scan_arguments({
            "network": "192.0.2.0/24", "start_ip": "192.0.2.1", "end_ip": "192.0.2.42",
        })
        with self.assertRaises(ValueError):
            desktop_module.validate_scan_arguments({
                "network": "192.0.0.0/16", "start_ip": "192.0.2.1", "end_ip": "192.0.2.42",
            })
        with self.assertRaises(ValueError):
            desktop_module.validate_scan_arguments({
                "network": "192.0.2.0/24", "start_ip": "192.0.2.42", "end_ip": "192.0.2.1",
            })


if __name__ == "__main__":
    unittest.main()
