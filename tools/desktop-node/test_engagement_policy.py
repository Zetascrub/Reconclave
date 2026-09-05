import pathlib
import tempfile
import time
import unittest

from engagement_policy import EngagementPolicy
from workspace_store import WorkspaceStore


class EngagementPolicyTests(unittest.TestCase):
    def make_policy(self, directory, delegation_key=None):
        root = pathlib.Path(directory)
        store = WorkspaceStore(root / "workspace.json")
        project = store.create_project({"name": "Authorised lab"})
        return store, project, EngagementPolicy(store, root / "scope.key", delegation_key)

    def create_scope(self, policy, project):
        return policy.create_scope({"project_id": project["id"],
                                    "included_networks": ["192.168.10.0/24"],
                                    "excluded_networks": ["192.168.10.128/25"],
                                    "capability_classes": ["discovery", "vulnerability"],
                                    "expires_at_ms": int(time.time() * 1000) + 60000})

    def test_signed_scope_allows_included_target_and_rejects_exclusion(self):
        with tempfile.TemporaryDirectory() as directory:
            _, project, policy = self.make_policy(directory)
            scope = self.create_scope(policy, project)
            policy.authorize(scope["id"], project["id"], "net.discovery.scan",
                             {"network": "192.168.10.0/26"})
            with self.assertRaisesRegex(PermissionError, "outside"):
                policy.authorize(scope["id"], project["id"], "vuln.scan",
                                 {"host": "192.168.10.200"})

    def test_tampered_and_expired_scopes_are_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            store, project, policy = self.make_policy(directory)
            scope = self.create_scope(policy, project)
            store.data["scopes"][0]["included_networks"] = ["0.0.0.0/0"]
            with self.assertRaisesRegex(PermissionError, "signature"):
                policy.get_valid(scope["id"], project["id"])

    def test_scope_revisions_are_immutable_and_incrementing(self):
        with tempfile.TemporaryDirectory() as directory:
            store, project, policy = self.make_policy(directory)
            first = self.create_scope(policy, project)
            second = self.create_scope(policy, project)
            self.assertEqual((first["revision"], second["revision"]), (1, 2))
            self.assertEqual(len(store.snapshot()["audit_events"]), 2)

    def test_delegation_is_argument_bound_and_concurrency_limited(self):
        with tempfile.TemporaryDirectory() as directory:
            store, project, policy = self.make_policy(directory, b"k" * 32)
            scope = policy.create_scope({"project_id": project["id"],
                                         "included_networks": ["192.168.10.0/24"],
                                         "excluded_networks": [],
                                         "capability_classes": ["discovery"],
                                         "max_concurrency": 1,
                                         "expires_at_ms": int(time.time() * 1000) + 60000})
            delegated = policy.delegate(scope["id"], project["id"], "tool.nmap.services",
                                        {"hosts": ["192.168.10.2"], "ports": [443]}, "node-1")
            self.assertIn("_scope_delegation", delegated)
            with self.assertRaisesRegex(PermissionError, "concurrency"):
                policy.delegate(scope["id"], project["id"], "tool.nmap.services",
                                {"hosts": ["192.168.10.3"], "ports": [443]}, "node-1")
            policy.release(delegated)
            policy.delegate(scope["id"], project["id"], "tool.nmap.services",
                            {"hosts": ["192.168.10.3"], "ports": [443]}, "node-1")


if __name__ == "__main__":
    unittest.main()
