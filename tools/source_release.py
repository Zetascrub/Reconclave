#!/usr/bin/env python3
"""Package committed public source only; never package a working deployment directory."""
import argparse
import os
from pathlib import Path, PurePosixPath
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]


def forbidden(path: str) -> bool:
    p = PurePosixPath(path)
    if p.name.startswith('.env') and not p.name.endswith('.example'):
        return True
    if p.name == 'generated_trust.h' or p.suffix in ('.bin', '.elf', '.key'):
        return True
    if p.suffix == '.pem' and not (path.startswith('release/keys/') and p.name.endswith('.pub.pem')):
        return True
    return any(part in {'evidence', '.reconclave-data', '.reconclave-provisioning',
                        '.reconclave-signing', '.reconclave-releases', '.pio', 'node_modules'} for part in p.parts)


def git(*args):
    return subprocess.check_output(['git', '-C', str(ROOT), *args])


def validate_tree(commit: str) -> None:
    entries = git('ls-tree', '-r', '-z', commit).split(b'\0')
    names = set()
    for entry in entries:
        if not entry:
            continue
        meta, raw_name = entry.split(b'\t', 1)
        mode, kind, oid = meta.split()
        name = raw_name.decode('utf-8')
        names.add(name)
        if mode not in (b'100644', b'100755') or kind != b'blob' or forbidden(name):
            raise ValueError('private/unsupported source path: ' + name)
        data = git('cat-file', 'blob', oid.decode())
        if any(line.strip().startswith(b'-----BEGIN ') and
               line.strip().endswith(b'PRIVATE KEY-----') for line in data.splitlines()):
            raise ValueError('private key material: ' + name)
    required = {'LICENSE', 'SECURITY.md', 'CONTRIBUTING.md', 'release/keys/reconclave-release.pub.pem'}
    if not required <= names:
        raise ValueError('missing release files: ' + ', '.join(sorted(required - names)))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--version', required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    try:
        if not re.fullmatch(r'[A-Za-z0-9._+-]{1,64}', args.version):
            raise ValueError('invalid version')
        if git('status', '--porcelain', '--untracked-files=normal').strip():
            raise ValueError('commit or remove pending changes before packaging')
        commit = git('rev-parse', 'HEAD').decode().strip()
        validate_tree(commit)
        data = git('archive', '--format=tar.gz', '--prefix=Reconclave-' + args.version + '/', commit)
        fd = os.open(args.output, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        with os.fdopen(fd, 'wb') as stream:
            stream.write(data)
        print('Source commit:', commit)
        print('Source archive:', args.output)
        print('Run secret scanning and sign this archive before publishing it.')
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        print(str(error), file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
