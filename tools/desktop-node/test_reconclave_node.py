import hashlib
import hmac
import importlib.util
import json
import pathlib
import sys
import tempfile
import types
import unittest
from unittest import mock


fake_zeroconf = types.ModuleType("zeroconf")
fake_zeroconf.ServiceInfo = object
fake_zeroconf.Zeroconf = object
sys.modules.setdefault("zeroconf", fake_zeroconf)
module_path = pathlib.Path(__file__).with_name("reconclave_node.py")
spec = importlib.util.spec_from_file_location("reconclave_desktop_node", module_path)
node_module = importlib.util.module_from_spec(spec)
assert spec.loader is not None
spec.loader.exec_module(node_module)


class DesktopNodeTests(unittest.TestCase):
    def make_request(self, node, capability, arguments, auth=None):
        payload = {"request_id": "req-1", "capability": capability, "arguments": arguments}
        if auth is not None:
            payload["auth"] = auth
        return {
            "proto": node_module.PROTOCOL,
            "type": "request",
            "source_node": "rc-test-coordinator",
            "destination_node": node.node_id,
            "payload": payload,
        }

    def test_capabilities_follow_enabled_handlers(self):
        plain = node_module.Node("plain", "plain", "127.0.0.1", 8767)
        scanning = node_module.Node("scan", "scan", "127.0.0.1", 8767, True,
                                    execution_key="test key")
        self.assertNotIn("net.discovery.scan", plain.announcement()["payload"]["capabilities"])
        self.assertIn("net.discovery.scan", scanning.announcement()["payload"]["capabilities"])
        scan_descriptor = next(item for item in scanning.announcement()["payload"]["capability_descriptors"]
                               if item["id"] == "net.discovery.scan")
        self.assertEqual(scan_descriptor["permission"], "trusted")
        self.assertEqual(scan_descriptor["limits"]["weight"], 4)
        self.assertNotIn("recurring", scan_descriptor["features"])
        with tempfile.TemporaryDirectory() as directory:
            durable = node_module.Node("durable", "durable", "127.0.0.1", 8767, True,
                                       evidence_dir=directory, execution_key="test key")
            durable_descriptor = next(item for item in durable.announcement()["payload"]["capability_descriptors"]
                                      if item["id"] == "net.discovery.scan")
            self.assertIn("durable", durable_descriptor["features"])
            self.assertIn("callback", durable_descriptor["features"])

    def test_callback_lease_verifies_expected_coordinator(self):
        node = node_module.Node("scan", "scan", "127.0.0.1", 8767)
        response = mock.MagicMock()
        response.__enter__.return_value = response
        response.__exit__.return_value = False
        with mock.patch.object(node_module.urllib.request, "urlopen", return_value=response), \
             mock.patch.object(node_module.json, "load", return_value={
                 "payload": {"device_id": "rc-coordinator", "roles": ["coordinator"]}
             }):
            self.assertTrue(node._callback_coordinator_reachable({
                "callback_endpoint": "http://192.0.2.1:8766",
                "owner_coordinator": "rc-coordinator",
            }))
            self.assertFalse(node._callback_coordinator_reachable({
                "callback_endpoint": "http://192.0.2.1:8766",
                "owner_coordinator": "different-node",
            }))

    def test_network_scan_is_not_exposed_without_authentication_key(self):
        node = node_module.Node("scan", "scan", "127.0.0.1", 8767, True)
        self.assertNotIn("net.discovery.scan", node.announcement()["payload"]["capabilities"])

    def test_announcement_creates_new_evidence_directory_before_reporting_resources(self):
        with tempfile.TemporaryDirectory() as parent:
            directory = str(pathlib.Path(parent) / "new-evidence")
            node = node_module.Node("collector", "collector", "127.0.0.1", 8767,
                                    evidence_dir=directory, evidence_key="test key")
            announcement = node.announcement()
            self.assertTrue(pathlib.Path(directory).is_dir())
            self.assertTrue(announcement["payload"]["resources"]["persistent_storage"])

    def test_non_object_request_and_arguments_are_rejected(self):
        node = node_module.Node("node", "node", "127.0.0.1", 8767)
        _, response = node.respond([])
        self.assertEqual(response["payload"]["error"]["code"], "INVALID_REQUEST")
        request = self.make_request(node, "system.info", [])
        _, response = node.respond(request)
        self.assertEqual(response["payload"]["error"]["code"], "INVALID_REQUEST")

    def test_authenticated_evidence_write_and_replay_rejection(self):
        with tempfile.TemporaryDirectory() as directory:
            passphrase = "test evidence key"
            node = node_module.Node("collector", "collector", "127.0.0.1", 8767,
                                    evidence_dir=directory, evidence_key=passphrase)
            evidence = {
                "job_id": "job-1", "source_node": "rc-test-coordinator",
                "evidence_id": "evidence-1",
                "target": "192.0.2.10", "timestamp_ms": 1,
                "observation": {"responsive": True},
            }
            nonce = "0011223344556677"
            canonical = f"rc-test-coordinator|collector|req-1|storage.evidence.write|{nonce}".encode()
            key = hashlib.sha256(passphrase.encode()).digest()
            tag = hmac.new(key, canonical, hashlib.sha256).digest()[:16].hex()
            request = self.make_request(node, "storage.evidence.write", {"evidence": evidence},
                                        {"nonce": nonce, "tag": tag})
            _, response = node.respond(request)
            self.assertEqual(response["payload"]["status"], "ok")
            self.assertEqual(response["payload"]["result"]["evidence_id"], "evidence-1")
            records = list(pathlib.Path(directory).glob("evidence-*.jsonl"))
            self.assertEqual(len(records), 1)
            self.assertEqual(json.loads(records[0].read_text()), evidence)
            _, replay = node.respond(request)
            self.assertEqual(replay["payload"]["error"]["code"], "UNAUTHENTICATED")

            second_nonce = "0011223344556688"
            second_canonical = (f"rc-test-coordinator|collector|req-1|storage.evidence.write|"
                                f"{second_nonce}").encode()
            second_tag = hmac.new(key, second_canonical, hashlib.sha256).digest()[:16].hex()
            request["payload"]["auth"] = {"nonce": second_nonce, "tag": second_tag}
            _, duplicate = node.respond(request)
            self.assertTrue(duplicate["payload"]["result"]["duplicate"])
            self.assertEqual(len(records[0].read_text().splitlines()), 1)


if __name__ == "__main__":
    unittest.main()
