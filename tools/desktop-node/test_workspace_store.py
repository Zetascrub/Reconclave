import pathlib
import tempfile
import unittest

from workspace_store import WorkspaceStore


class WorkspaceStoreTests(unittest.TestCase):
    def test_project_job_and_evidence_survive_restart(self):
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "workspace.json"
            store = WorkspaceStore(path)
            project = store.create_project({"name": "Lab", "description": "Authorised range"})
            job = store.upsert_job({"id": "job-1", "project_id": project["id"],
                                    "provider_id": "rc-p4", "capability": "net.discovery.scan",
                                    "status": "running", "checked": 2, "total": 10})
            store.upsert_job({**job, "status": "complete", "hosts": ["192.0.2.4"]})
            evidence = store.add_evidence({"id": "ev-1", "project_id": project["id"],
                                           "job_id": "job-1", "kind": "network-hosts",
                                           "data": {"hosts": ["192.0.2.4"]}})
            self.assertEqual(evidence["id"], "ev-1")
            restored = WorkspaceStore(path).snapshot()
            self.assertEqual(restored["jobs"][0]["status"], "complete")
            self.assertEqual(restored["evidence"][0]["data"]["hosts"], ["192.0.2.4"])

            rule = store.create_automation({"project_id": project["id"], "node_id": "rc-p4",
                                            "condition": "dhcp_assigned",
                                            "playbook": "system_snapshot", "interval_ms": 0})
            store.set_automation(rule["id"], {"enabled": False})
            self.assertFalse(WorkspaceStore(path).snapshot()["automations"][0]["enabled"])
            store.delete_automation(rule["id"])
            self.assertEqual(store.snapshot()["automations"], [])

    def test_rejects_job_for_unknown_project(self):
        with tempfile.TemporaryDirectory() as directory:
            store = WorkspaceStore(pathlib.Path(directory) / "workspace.json")
            with self.assertRaisesRegex(ValueError, "project does not exist"):
                store.upsert_job({"id": "job-1", "project_id": "missing"})

    def test_automation_rejects_arbitrary_payloads(self):
        with tempfile.TemporaryDirectory() as directory:
            store = WorkspaceStore(pathlib.Path(directory) / "workspace.json")
            project = store.create_project({"name": "Lab"})
            with self.assertRaisesRegex(ValueError, "unsupported automation playbook"):
                store.create_automation({"project_id": project["id"], "node_id": "rc-p4",
                                         "condition": "dhcp_assigned", "playbook": "shell"})


if __name__ == "__main__":
    unittest.main()
