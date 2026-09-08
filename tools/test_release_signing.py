import json
import os
from pathlib import Path
import tempfile
import unittest
from cryptography.exceptions import InvalidSignature
import release_signing as signing


class SigningTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.private = self.root / 'private.pem'
        self.public = self.root / 'public.pem'
        self.manifest = self.root / 'release.json'
        self.artifact = self.root / 'firmware.bin'
        self.artifact.write_bytes(b'firmware test bytes')
        signing.init_key(self.private, self.public)
        signing.sign(self.artifact, self.private, self.manifest, '0.1.0', 'cardputer-adv',
                     'a' * 40, 'private-deployment-firmware')

    def verify(self, **kwargs):
        return signing.verify(self.artifact, kwargs.get('public', self.public), self.manifest,
                              kwargs.get('target', 'cardputer-adv'), kwargs.get('version', '0.1.0'))

    def test_round_trip_and_key_permissions(self):
        self.assertEqual(self.verify()['kind'], 'private-deployment-firmware')
        self.assertEqual(self.private.stat().st_mode & 0o777, 0o600)

    def test_changed_artifact(self):
        self.artifact.write_bytes(b'altered firmware')
        with self.assertRaises(ValueError): self.verify()

    def test_changed_metadata(self):
        value = json.loads(self.manifest.read_text())
        value['manifest']['version'] = '9.9.9'
        self.manifest.write_text(json.dumps(value))
        with self.assertRaises(InvalidSignature): self.verify(version='9.9.9')

    def test_wrong_target_version_and_key(self):
        for args in ({'target': 'poe-p4'}, {'version': '0.2.0'}):
            with self.assertRaises(ValueError): self.verify(**args)
        other = self.root / 'other.pub'
        signing.init_key(self.root / 'other.key', other)
        with self.assertRaises(InvalidSignature): self.verify(public=other)

    def test_keygen_preserves_existing_keys(self):
        before = self.private.read_bytes()
        signing.init_key(self.private, self.public)
        self.assertEqual(before, self.private.read_bytes())

    def test_refuses_insecure_private_permissions(self):
        os.chmod(self.private, 0o644)
        with self.assertRaises(ValueError): signing.private_key(self.private)

    def test_duplicate_json_fields(self):
        self.manifest.write_text('{"manifest": {}, "manifest": {}, "signature": "00"}')
        with self.assertRaises(ValueError): self.verify()

    def test_will_not_overwrite_manifest(self):
        with self.assertRaises(FileExistsError):
            signing.sign(self.artifact, self.private, self.manifest, '0.1.0', 'cardputer-adv',
                         'a' * 40, 'private-deployment-firmware')

    def test_firmware_cannot_be_labelled_public_source(self):
        with self.assertRaises(ValueError):
            signing.sign(self.artifact, self.private, self.root / 'other.json', '0.1.0',
                         'cardputer-adv', 'a' * 40, 'source')

    def test_dirty_source_is_signed_and_visible(self):
        manifest = self.root / 'dirty.json'
        signing.sign(self.artifact, self.private, manifest, '0.1.0', 'cardputer-adv',
                     'a' * 40, 'private-deployment-firmware', dirty_source=True)
        self.assertTrue(signing.verify(self.artifact, self.public, manifest, 'cardputer-adv')['source_dirty'])
        with self.assertRaises(ValueError):
            signing.sign(self.artifact, self.private, self.root / 'public.json', '0.1.0',
                         'source', 'a' * 40, 'source', dirty_source=True)


if __name__ == '__main__': unittest.main()
