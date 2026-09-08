import unittest
import tempfile
from pathlib import Path
import subprocess
import source_release
from source_release import forbidden


class SourcePolicyTests(unittest.TestCase):
    def test_private_paths_blocked(self):
        for path in ('.env', 'app/.env.production', 'devices/a/generated_trust.h',
                     'release/firmware.bin', 'release/firmware.elf', 'private.pem',
                     'app/evidence/capture.jsonl', '.reconclave-provisioning/fleet.json'):
            with self.subTest(path=path): self.assertTrue(forbidden(path))

    def test_public_source_and_examples_allowed(self):
        for path in ('release/keys/reconclave-release.pub.pem', '.env.example',
                     'protocol/schemas/evidence.schema.json', 'tools/release_signing.py'):
            with self.subTest(path=path): self.assertFalse(forbidden(path))


class SourceTreeTests(unittest.TestCase):
    def test_committed_tree_blocks_private_material(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            previous = source_release.ROOT
            source_release.ROOT = root
            self.addCleanup(setattr, source_release, 'ROOT', previous)
            subprocess.run(['git', 'init', '-q', str(root)], check=True)
            def commit():
                subprocess.run(['git', '-C', str(root), 'add', '.'], check=True)
                subprocess.run(['git', '-C', str(root), '-c', 'user.name=Test', '-c',
                                'user.email=test@example.invalid', 'commit', '-qm', 'fixture'], check=True)
                return source_release.git('rev-parse', 'HEAD').decode().strip()
            for name in ('LICENSE', 'SECURITY.md', 'CONTRIBUTING.md', 'release/keys/reconclave-release.pub.pem'):
                path = root / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text('test fixture')
            source_release.validate_tree(commit())
            (root / 'firmware.bin').write_bytes(b'private firmware')
            with self.assertRaises(ValueError): source_release.validate_tree(commit())
            (root / 'firmware.bin').unlink()
            (root / 'notes.txt').write_text('-----BEGIN PRIVATE KEY-----\nsecret\n')
            with self.assertRaises(ValueError): source_release.validate_tree(commit())
