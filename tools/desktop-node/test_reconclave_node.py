import hashlib
import hmac
import importlib.util
import json
import pathlib
import sys
import tempfile
import types
import unittest


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
        scanning = node_module.Node("scan", "scan", "127.0.0.1", 8767, True)
        self.assertNotIn("net.discovery.scan", plain.announcement()["payload"]["capabilities"])
        self.assertIn("net.discovery.scan", scanning.announcement()["payload"]["capabilities"])

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
            records = list(pathlib.Path(directory).glob("evidence-*.jsonl"))
            self.assertEqual(len(records), 1)
            self.assertEqual(json.loads(records[0].read_text()), evidence)
            _, replay = node.respond(request)
            self.assertEqual(replay["payload"]["error"]["code"], "UNAUTHENTICATED")


if __name__ == "__main__":
    unittest.main()
