import pathlib
import tempfile
import unittest
from unittest import mock

from fleet_manager import FleetManager
from workspace_store import WorkspaceStore


ARTIFACT_SHA = "a" * 64
PREVIOUS_ARTIFACT_SHA = "b" * 64


class FleetManagerTests(unittest.TestCase):
    def make_manager(self, directory):
        store = WorkspaceStore(pathlib.Path(directory) / "workspace.json")
        coordinator = mock.MagicMock()
        manager = FleetManager(coordinator, store, b"k" * 32)
        return manager, store, coordinator

    # -- config / drift -------------------------------------------------

    def test_set_config_rejects_unsupported_fields_and_bad_interval(self):
        with tempfile.TemporaryDirectory() as directory:
            manager, _store, _coordinator = self.make_manager(directory)
            manager.set_config({"device_type": "poe-p4", "desired": {"labels": ["lab"]}})
            with self.assertRaises(ValueError):
                manager.set_config({"device_type": "poe-p4", "desired": {"shell_command": "rm -rf /"}})
            with self.assertRaises(ValueError):
                manager.set_config({"device_type": "poe-p4",
                                    "desired": {"telemetry_interval_seconds": 1}})

    def test_reconcile_detects_capability_drift(self):
        with tempfile.TemporaryDirectory() as directory:
            manager, store, coordinator = self.make_manager(directory)
            manager.set_config({"device_type": "poe-p4",
                                "desired": {"enabled_capabilities": ["system.info", "net.arp.snapshot"]}})
            coordinator.state.return_value = {"nodes": [
                {"device_id": "rc-p4-01", "device_type": "poe-p4", "status": "ready",
                 "firmware": "0.1.0", "address": "192.168.1.5", "capabilities": ["system.info"]},
            ]}
            fleet = manager.reconcile_once()
            [node] = fleet
            self.assertIn("enabled_capabilities", node["drift"])
            self.assertEqual(node["drift"]["enabled_capabilities"]["desired"],
                             ["system.info", "net.arp.snapshot"])
            self.assertEqual(store.snapshot()["fleet_nodes"], fleet)

    def test_reconcile_preserves_first_seen_across_ticks(self):
        with tempfile.TemporaryDirectory() as directory:
            manager, _store, coordinator = self.make_manager(directory)
            coordinator.state.return_value = {"nodes": [
                {"device_id": "rc-adv-01", "device_type": "cardputer-adv", "status": "ready",
                 "capabilities": []},
            ]}
            first = manager.reconcile_once()[0]["first_seen_ms"]
            coordinator.state.return_value = {"nodes": [
                {"device_id": "rc-adv-01", "device_type": "cardputer-adv", "status": "ready",
                 "capabilities": ["radio.ble.scan"]},
            ]}
            second = manager.reconcile_once()[0]
            self.assertEqual(second["first_seen_ms"], first)
            self.assertGreaterEqual(second["last_seen_ms"], first)

    # -- signed releases --------------------------------------------------

    def test_create_release_requires_safe_fields_and_signs_the_manifest(self):
        with tempfile.TemporaryDirectory() as directory:
            manager, _store, _coordinator = self.make_manager(directory)
            with self.assertRaises(ValueError):
                manager.create_release({"device_type": "poe-p4", "version": "1.0.0",
                                        "artifact_sha256": "not-hex"})
            with self.assertRaises(ValueError):
                manager.create_release({"device_type": "", "version": "1.0.0",
                                        "artifact_sha256": ARTIFACT_SHA})
            release = manager.create_release({"device_type": "poe-p4", "version": "1.0.0",
                                              "artifact_sha256": ARTIFACT_SHA})
            self.assertEqual(len(release["signature"]), 64)
            other_manager, _, _ = self.make_manager(directory)
            # A signature computed with a different key must not match by chance.
            forged = {**release}
            forged["signature"] = other_manager.create_release({
                "device_type": "poe-p4", "version": "1.0.1", "artifact_sha256": ARTIFACT_SHA})["signature"]
            self.assertNotEqual(release["signature"], forged["signature"])

    # -- staged rollout: batching, verification, rollback trigger --------

    def _staged_rollout(self, manager, store, coordinator, node_count=3, batch_size=2):
        release = manager.create_release({"device_type": "poe-p4", "version": "1.0.0",
                                          "artifact_sha256": ARTIFACT_SHA})
        coordinator.state.return_value = {"nodes": [
            {"device_id": f"rc-p4-{index}", "device_type": "poe-p4", "status": "ready",
             "capabilities": ["fleet.ota.apply"]}
            for index in range(node_count)
        ]}
        manager.reconcile_once()
        rollout = manager.create_rollout({"release_id": release["id"], "batch_size": batch_size})
        self.assertEqual(len(rollout["targets"]), node_count)
        self.assertEqual(rollout["status"], "staged")
        return release, rollout

    def test_rollout_batches_and_verifies_matching_artifact(self):
        with tempfile.TemporaryDirectory() as directory:
            manager, store, coordinator = self.make_manager(directory)
            release, rollout = self._staged_rollout(manager, store, coordinator, node_count=3, batch_size=2)
            coordinator.invoke.side_effect = [
                {"payload": {"result": {"artifact_sha256": ARTIFACT_SHA}}},
                {"payload": {"result": {"artifact_sha256": ARTIFACT_SHA}}},
            ]
            advanced = manager.advance_rollout(rollout["id"])
            statuses = [item["status"] for item in advanced["targets"]]
            self.assertEqual(statuses.count("verified"), 2)
            self.assertEqual(statuses.count("pending"), 1)
            self.assertEqual(advanced["status"], "running")
            # Second batch: only the last pending target remains.
            coordinator.invoke.side_effect = [{"payload": {"result": {"artifact_sha256": ARTIFACT_SHA}}}]
            final = manager.advance_rollout(rollout["id"])
            self.assertEqual(final["status"], "complete")
            self.assertTrue(all(item["status"] == "verified" for item in final["targets"]))

    def test_rollout_marks_ineligible_nodes_without_ota_capability(self):
        with tempfile.TemporaryDirectory() as directory:
            manager, store, coordinator = self.make_manager(directory)
            release = manager.create_release({"device_type": "poe-p4", "version": "1.0.0",
                                              "artifact_sha256": ARTIFACT_SHA})
            coordinator.state.return_value = {"nodes": [
                {"device_id": "rc-p4-0", "device_type": "poe-p4", "status": "ready", "capabilities": []},
            ]}
            manager.reconcile_once()
            rollout = manager.create_rollout({"release_id": release["id"], "batch_size": 4})
            advanced = manager.advance_rollout(rollout["id"])
            self.assertEqual(advanced["targets"][0]["status"], "ineligible")
            coordinator.invoke.assert_not_called()

    def test_rollout_flags_verification_mismatch_as_failed(self):
        with tempfile.TemporaryDirectory() as directory:
            manager, store, coordinator = self.make_manager(directory)
            release, rollout = self._staged_rollout(manager, store, coordinator, node_count=1, batch_size=4)
            coordinator.invoke.side_effect = [{"payload": {"result": {"artifact_sha256": "wrong"}}}]
            advanced = manager.advance_rollout(rollout["id"])
            self.assertEqual(advanced["targets"][0]["status"], "failed")
            self.assertIn("mismatch", advanced["targets"][0]["error"])

    def test_rollout_requires_rollback_once_failure_threshold_exceeded(self):
        with tempfile.TemporaryDirectory() as directory:
            manager, store, coordinator = self.make_manager(directory)
            release, rollout = self._staged_rollout(manager, store, coordinator, node_count=4, batch_size=4)
            # 3 of 4 fail -> 75% > the 25% failure_threshold.
            coordinator.invoke.side_effect = [
                {"payload": {"result": {"artifact_sha256": "wrong"}}},
                {"payload": {"result": {"artifact_sha256": "wrong"}}},
                {"payload": {"result": {"artifact_sha256": "wrong"}}},
                {"payload": {"result": {"artifact_sha256": ARTIFACT_SHA}}},
            ]
            advanced = manager.advance_rollout(rollout["id"])
            self.assertEqual(advanced["status"], "rollback_required")

    def test_rollout_partial_when_some_fail_under_threshold(self):
        with tempfile.TemporaryDirectory() as directory:
            manager, store, coordinator = self.make_manager(directory)
            release, rollout = self._staged_rollout(manager, store, coordinator, node_count=4, batch_size=4)
            coordinator.invoke.side_effect = [
                {"payload": {"result": {"artifact_sha256": "wrong"}}},
                {"payload": {"result": {"artifact_sha256": ARTIFACT_SHA}}},
                {"payload": {"result": {"artifact_sha256": ARTIFACT_SHA}}},
                {"payload": {"result": {"artifact_sha256": ARTIFACT_SHA}}},
            ]
            advanced = manager.advance_rollout(rollout["id"])
            self.assertEqual(advanced["status"], "partial")

    # -- rollback ----------------------------------------------------------

    def test_rollback_requires_a_previous_release(self):
        with tempfile.TemporaryDirectory() as directory:
            manager, store, coordinator = self.make_manager(directory)
            release, rollout = self._staged_rollout(manager, store, coordinator, node_count=1, batch_size=4)
            coordinator.invoke.side_effect = [{"payload": {"result": {"artifact_sha256": ARTIFACT_SHA}}}]
            manager.advance_rollout(rollout["id"])
            with self.assertRaisesRegex(ValueError, "no prior release"):
                manager.rollback_rollout(rollout["id"])

    def test_rollback_reverts_verified_targets_to_the_prior_release(self):
        with tempfile.TemporaryDirectory() as directory:
            manager, store, coordinator = self.make_manager(directory)
            first_release = manager.create_release({"device_type": "poe-p4", "version": "1.0.0",
                                                     "artifact_sha256": PREVIOUS_ARTIFACT_SHA})
            coordinator.state.return_value = {"nodes": [
                {"device_id": "rc-p4-0", "device_type": "poe-p4", "status": "ready",
                 "capabilities": ["fleet.ota.apply"]},
                {"device_id": "rc-p4-1", "device_type": "poe-p4", "status": "ready",
                 "capabilities": ["fleet.ota.apply"]},
            ]}
            manager.reconcile_once()
            first_rollout = manager.create_rollout({"release_id": first_release["id"], "batch_size": 4})
            self.assertEqual(first_rollout["previous_release_id"], "")
            coordinator.invoke.side_effect = [
                {"payload": {"result": {"artifact_sha256": PREVIOUS_ARTIFACT_SHA}}},
                {"payload": {"result": {"artifact_sha256": PREVIOUS_ARTIFACT_SHA}}},
            ]
            manager.advance_rollout(first_rollout["id"])  # -> complete, establishes prior release

            second_release = manager.create_release({"device_type": "poe-p4", "version": "2.0.0",
                                                      "artifact_sha256": ARTIFACT_SHA})
            second_rollout = manager.create_rollout({"release_id": second_release["id"], "batch_size": 4})
            self.assertEqual(second_rollout["previous_release_id"], first_release["id"])
            coordinator.invoke.side_effect = [
                {"payload": {"result": {"artifact_sha256": ARTIFACT_SHA}}},
                {"payload": {"result": {"artifact_sha256": "wrong"}}},
            ]
            advanced = manager.advance_rollout(second_rollout["id"])
            self.assertEqual(advanced["status"], "rollback_required")

            coordinator.invoke.reset_mock()
            coordinator.invoke.side_effect = [
                {"payload": {"result": {"artifact_sha256": PREVIOUS_ARTIFACT_SHA}}},
            ]
            rolled_back = manager.rollback_rollout(second_rollout["id"])
            self.assertEqual(rolled_back["status"], "rolled_back")
            statuses = {item["device_id"]: item["status"] for item in rolled_back["targets"]}
            self.assertEqual(statuses["rc-p4-0"], "rolled_back")
            # This target was never "verified" on the new release, so it's left alone.
            self.assertEqual(statuses["rc-p4-1"], "failed")
            # The rollback call itself only invoked the coordinator for the one verified target.
            coordinator.invoke.assert_called_once_with("rc-p4-0", "fleet.ota.apply",
                                                        {"release": first_release})

    def test_rollback_marks_partial_when_a_revert_itself_fails(self):
        with tempfile.TemporaryDirectory() as directory:
            manager, store, coordinator = self.make_manager(directory)
            first_release = manager.create_release({"device_type": "poe-p4", "version": "1.0.0",
                                                     "artifact_sha256": PREVIOUS_ARTIFACT_SHA})
            coordinator.state.return_value = {"nodes": [
                {"device_id": "rc-p4-0", "device_type": "poe-p4", "status": "ready",
                 "capabilities": ["fleet.ota.apply"]},
            ]}
            manager.reconcile_once()
            first_rollout = manager.create_rollout({"release_id": first_release["id"], "batch_size": 4})
            coordinator.invoke.side_effect = [{"payload": {"result": {"artifact_sha256": PREVIOUS_ARTIFACT_SHA}}}]
            manager.advance_rollout(first_rollout["id"])

            second_release = manager.create_release({"device_type": "poe-p4", "version": "2.0.0",
                                                      "artifact_sha256": ARTIFACT_SHA})
            second_rollout = manager.create_rollout({"release_id": second_release["id"], "batch_size": 4})
            coordinator.invoke.side_effect = [{"payload": {"result": {"artifact_sha256": ARTIFACT_SHA}}}]
            manager.advance_rollout(second_rollout["id"])

            coordinator.invoke.side_effect = RuntimeError("node unreachable")
            rolled_back = manager.rollback_rollout(second_rollout["id"])
            self.assertEqual(rolled_back["status"], "rollback_partial")
            self.assertEqual(rolled_back["targets"][0]["status"], "rollback_failed")

    def test_fleet_state_survives_restart(self):
        with tempfile.TemporaryDirectory() as directory:
            manager, store, coordinator = self.make_manager(directory)
            release = manager.create_release({"device_type": "poe-p4", "version": "1.0.0",
                                              "artifact_sha256": ARTIFACT_SHA})
            coordinator.state.return_value = {"nodes": [
                {"device_id": "rc-p4-0", "device_type": "poe-p4", "status": "ready",
                 "capabilities": ["fleet.ota.apply"]},
            ]}
            manager.reconcile_once()
            rollout = manager.create_rollout({"release_id": release["id"], "batch_size": 4})
            restored = WorkspaceStore(store.path).snapshot()
            self.assertEqual(restored["ota_releases"][0]["id"], release["id"])
            self.assertEqual(restored["ota_rollouts"][0]["id"], rollout["id"])
            self.assertEqual(restored["fleet_nodes"][0]["device_id"], "rc-p4-0")


if __name__ == "__main__":
    unittest.main()
