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

    def test_rejects_job_for_unknown_project(self):
        with tempfile.TemporaryDirectory() as directory:
            store = WorkspaceStore(pathlib.Path(directory) / "workspace.json")
            with self.assertRaisesRegex(ValueError, "project does not exist"):
                store.upsert_job({"id": "job-1", "project_id": "missing"})


if __name__ == "__main__":
    unittest.main()
