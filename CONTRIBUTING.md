# Contributing

Reconclave is a development preview. Open a focused issue before proposing a
large feature. Keep changes bounded, explain the behaviour they change, and
include relevant validation and hardware limitations in the pull request.

Use systems you own or are authorised to assess. Report vulnerabilities using
[SECURITY.md](SECURITY.md), not a public bug report. Never contribute live
credentials, generated trust headers, provisioned firmware or assessment data.
Use synthetic test identities and targets.

## Keep contributions public-safe

Use synthetic examples for captures and test identities. Local keys, firmware
binaries, evidence, packet captures, databases and logs are excluded from normal
source changes. CI also rejects those paths if they are force-added. If a test
needs a fixture of a blocked type, discuss a narrow, reviewed exception instead
of weakening the repository-wide checks.

## Checks

```sh
.venv/bin/python -m unittest discover -s tools -p 'test_*.py'
python3 tools/check_public_tree.py
cmake -S . -B build
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Follow each linked device repository for its build and validation checks. No
production keys belong in CI or its artifacts.
List physical checks separately from mock/native tests. Do not claim hardware
coverage from a successful compilation.

See [release preparation](docs/releasing.md) for signing and publication rules.
