# Reconclave desktop node

This read-only Python node advertises itself over mDNS, serves a Reconclave v1
announcement, and implements `system.info`, `desktop.resources`, and bounded
local-network discovery (`net.discovery.scan` plus `coordination.job.status`
and `coordination.job.cancel`) capabilities. Its announcement is generated
from the live capability-handler registry, preventing it from advertising
handlers it does not provide. It does not execute commands or expose
unrestricted assessment capabilities.

Network discovery is restricted to an IPv4 `/24` or smaller. It checks a small
set of common TCP services and treats either a connection or an explicit refusal
as evidence that a host is present. Only scan networks you own or are authorised
to assess. It is disabled by default; explicitly enable it with:

```bash
.venv-desktop-node/bin/python tools/desktop-node/reconclave_node.py --enable-network-scan
```

When enabled, the desktop appears automatically among the Cardputer Scout
providers. Without the flag, those capabilities are neither advertised nor
callable.

`net.discovery.scan` accepts an optional `schedule: {"interval_ms": N,
"after_completion": true}` argument to run repeatedly until stopped with
`coordination.job.cancel`; `coordination.job.status` then reports `recurring`
and `run_count`. See `docs/capabilities.md` for the full contract.

Passing `--evidence-dir PATH` advertises `storage.evidence.write`, making this
node an Evidence Collector: any coordinator that knows about it will forward a
copy of evidence it produces or observes, appended as JSON Lines under
`PATH/evidence-YYYYMMDD.jsonl`. `storage.evidence.write` and
`coordination.job.cancel` can write state or stop a job, so **neither is
advertised without also passing `--evidence-key`** — a shared passphrase that
must match the coordinator's (on the Cardputer: Settings > Trust > Evidence
key). Every request to these two capabilities is signed and checked against
recently-seen nonces; an unsigned, wrongly-signed, or replayed request is
rejected with `UNAUTHENTICATED`. See "Authenticated capabilities" in
`docs/capabilities.md` for the exact signature scheme.

```bash
.venv-desktop-node/bin/python tools/desktop-node/reconclave_node.py \
    --evidence-dir ~/reconclave-evidence --evidence-key "correct horse battery staple"
```

Create an isolated environment and run it from the repository root:

```bash
python3 -m venv .venv-desktop-node
.venv-desktop-node/bin/pip install -r tools/desktop-node/requirements.txt
.venv-desktop-node/bin/python tools/desktop-node/reconclave_node.py
```

Open **Reconclave** on the Cardputer and use its refresh action. The machine
should appear as `desktop-node`. The default identity is stable for that
machine; override it with `--node-id` if required.

If the wrong interface is selected on a multi-homed computer, pass its LAN
address explicitly:

```bash
.venv-desktop-node/bin/python tools/desktop-node/reconclave_node.py --address 192.168.1.20
```

The host firewall must permit inbound TCP on port 8767 and mDNS/UDP 5353 on the
local network. Use `Ctrl-C` for a clean shutdown.
