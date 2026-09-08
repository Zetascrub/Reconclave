#!/usr/bin/env python3
"""Check committed source for private files without building or publishing artifacts."""
import subprocess
import sys
from source_release import git, validate_tree


def main():
    try:
        commit = git('rev-parse', 'HEAD').decode().strip()
        validate_tree(commit)
        print('Public source policy passed for', commit)
        return 0
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        print('Public source policy failed:', error, file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
