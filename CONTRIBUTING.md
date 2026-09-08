# Contributing

Reconclave is a development preview. Open a focused issue before proposing a
large feature. Keep changes bounded, explain the behaviour they change, and
include relevant validation and hardware limitations in the pull request.

Use systems you own or are authorised to assess. Report vulnerabilities using
[SECURITY.md](SECURITY.md), not a public bug report. Never contribute live
credentials, generated trust headers, provisioned firmware or assessment data.
Use synthetic test identities and targets.

## Checks

```sh
python3 -m venv .venv
.venv/bin/pip install -r tools/desktop-node/requirements.txt
.venv/bin/python -m unittest discover -s tools -p 'test_*.py'
(cd tools/desktop-node && ../../.venv/bin/python -m unittest discover -p 'test_*.py')
cmake -S . -B build
cmake --build build --parallel
ctest --test-dir build --output-on-failure
(cd tools/desktop-node/web && npm ci && npm run build)
```

Follow each device guide for compilation with private, locally generated trust.
CI generates disposable keys; no production keys belong in CI or its artifacts.
List physical checks separately from mock/native tests. Do not claim hardware
coverage from a successful compilation.

See [release preparation](docs/releasing.md) for signing and publication rules.
