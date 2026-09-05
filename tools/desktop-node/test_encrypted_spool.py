import pathlib
import tempfile
import unittest

from cryptography.exceptions import InvalidTag

from encrypted_spool import EncryptedSpool


class EncryptedSpoolTests(unittest.TestCase):
    def test_round_trip_and_wrong_key_rejection(self):
        with tempfile.TemporaryDirectory() as directory:
            spool = EncryptedSpool(directory, "correct", "node-1")
            path = spool.append({"secret": "observation"}, "20260904")
            self.assertNotIn("observation", path.read_text())
            self.assertEqual(spool.read(path), [{"secret": "observation"}])
            with self.assertRaises(InvalidTag):
                EncryptedSpool(directory, "wrong", "node-1").read(path)

    def test_frame_cannot_be_moved_to_another_node(self):
        with tempfile.TemporaryDirectory() as directory:
            path = EncryptedSpool(directory, "key", "node-1").append({"value": 1}, "20260904")
            with self.assertRaisesRegex(ValueError, "identity"):
                EncryptedSpool(directory, "key", "node-2").read(path)


if __name__ == "__main__":
    unittest.main()
