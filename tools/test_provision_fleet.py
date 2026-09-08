import importlib.util
import json
import pathlib
import sys
import tempfile
import unittest

HERE = pathlib.Path(__file__).parent
spec = importlib.util.spec_from_file_location("provision_fleet", HERE / "provision_fleet.py")
provision_fleet = importlib.util.module_from_spec(spec)
assert spec.loader is not None
spec.loader.exec_module(provision_fleet)


class ProvisionFleetTests(unittest.TestCase):
    def run_provision(self, directory: pathlib.Path, rotate: bool = False) -> None:
        provision_fleet.STORE = directory / "fleet.json"
        provision_fleet.P4_HEADER = directory / "p4.h"
        provision_fleet.CARD_HEADER = directory / "card.h"
        argv = ["prog", "--desktop-id", "rc-desktop-1", "--p4-id", "rc-p4-1",
                "--cardputer-id", "rc-adv-1"]
        if rotate:
            argv.append("--rotate")
        old_argv = sys.argv
        sys.argv = argv
        try:
            provision_fleet.main()
        finally:
            sys.argv = old_argv

    def test_first_run_generates_link_and_storage_keys(self):
        with tempfile.TemporaryDirectory() as directory:
            directory = pathlib.Path(directory)
            self.run_provision(directory)
            fleet = json.loads(provision_fleet.STORE.read_text())
            self.assertEqual(set(fleet["storage_keys"]), {"rc-p4-1", "rc-adv-1"})
            for key in fleet["storage_keys"].values():
                self.assertRegex(key, r"^[0-9a-f]{64}$")
            self.assertIn("RC_STORAGE_KEY", provision_fleet.P4_HEADER.read_text())
            self.assertIn("RC_STORAGE_KEY", provision_fleet.CARD_HEADER.read_text())

    def test_storage_keys_differ_per_device(self):
        with tempfile.TemporaryDirectory() as directory:
            self.run_provision(pathlib.Path(directory))
            fleet = json.loads(provision_fleet.STORE.read_text())
            self.assertNotEqual(fleet["storage_keys"]["rc-p4-1"], fleet["storage_keys"]["rc-adv-1"])

    def test_rerun_without_rotate_preserves_links_and_storage_keys(self):
        with tempfile.TemporaryDirectory() as directory:
            directory = pathlib.Path(directory)
            self.run_provision(directory)
            first = json.loads(provision_fleet.STORE.read_text())
            self.run_provision(directory)
            second = json.loads(provision_fleet.STORE.read_text())
            self.assertEqual(first["links"], second["links"])
            self.assertEqual(first["storage_keys"], second["storage_keys"])

    def test_rotate_changes_links_and_storage_keys(self):
        with tempfile.TemporaryDirectory() as directory:
            directory = pathlib.Path(directory)
            self.run_provision(directory)
            first = json.loads(provision_fleet.STORE.read_text())
            self.run_provision(directory, rotate=True)
            second = json.loads(provision_fleet.STORE.read_text())
            self.assertNotEqual(first["links"], second["links"])
            self.assertNotEqual(first["storage_keys"], second["storage_keys"])

    def test_backfill_adds_storage_keys_to_a_pre_existing_store_without_touching_links(self):
        with tempfile.TemporaryDirectory() as directory:
            directory = pathlib.Path(directory)
            self.run_provision(directory)
            fleet = json.loads(provision_fleet.STORE.read_text())
            del fleet["storage_keys"]
            provision_fleet.STORE.write_text(json.dumps(fleet))
            self.run_provision(directory)
            backfilled = json.loads(provision_fleet.STORE.read_text())
            self.assertEqual(set(backfilled["storage_keys"]), {"rc-p4-1", "rc-adv-1"})
            self.assertEqual(fleet["links"], backfilled["links"])

    def test_headers_have_private_permissions(self):
        with tempfile.TemporaryDirectory() as directory:
            self.run_provision(pathlib.Path(directory))
            for path in (provision_fleet.STORE, provision_fleet.P4_HEADER, provision_fleet.CARD_HEADER):
                self.assertEqual(path.stat().st_mode & 0o777, 0o600)

    def test_invalid_existing_store_is_not_modified(self):
        with tempfile.TemporaryDirectory() as directory:
            self.run_provision(pathlib.Path(directory))
            fleet = json.loads(provision_fleet.STORE.read_text())
            fleet['links'][next(iter(fleet['links']))] = 'invalid'
            original = json.dumps(fleet)
            provision_fleet.STORE.write_text(original)
            with self.assertRaises(SystemExit): self.run_provision(pathlib.Path(directory))
            self.assertEqual(provision_fleet.STORE.read_text(), original)


if __name__ == "__main__":
    unittest.main()
