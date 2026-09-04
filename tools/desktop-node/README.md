# Reconclave desktop

## Web coordinator

The desktop web application is the recommended interactive runtime. It acts as
a node, coordinator, or both; discovers `_reconclave._tcp.local` peers; expires
stale nodes after 45 seconds; presents capability and resource metadata; and
can invoke safe inspection capabilities from a live node detail view.

Build the React interface once, then start the local application:

```bash
cd tools/desktop-node/web
npm install
npm run build
cd ..
../../.venv-desktop-node/bin/python desktop_app.py --mode both
```

Open <http://127.0.0.1:8767>. The live roster and command API are deliberately
loopback-only. The Reconclave announcement and message endpoints remain
available to other nodes on the LAN.

Trusted capability requests can be enabled without placing secrets in shell
history by using environment variables:

```bash
RECONCLAVE_EXECUTION_KEY="..." RECONCLAVE_EVIDENCE_KEY="..." \
  ../../.venv-desktop-node/bin/python desktop_app.py --mode both \
  --enable-network-scan --evidence-dir ./evidence
```

The current UI directly invokes `system.info`, `desktop.resources`, and
`coordination.job.status`. Assessment capabilities are visible but remain
disabled until their scope/configuration workflow is implemented.

For UI development, run `npm run dev` in `web/` while `desktop_app.py` is
running; Vite proxies `/api` requests to port 8767.

## Headless node

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
.venv-desktop-node/bin/python tools/desktop-node/reconclave_node.py \
    --enable-network-scan --execution-key "shared execution passphrase"
```

When enabled, the desktop appears automatically among the Cardputer Scout
providers. The scan capability is only registered when both the flag and a
execution key are present; otherwise it is neither advertised nor callable.

`net.discovery.scan` accepts an optional `schedule: {"interval_ms": N,
"after_completion": true}` argument to run repeatedly until stopped with
`coordination.job.cancel`; `coordination.job.status` then reports `recurring`
and `run_count`. See `docs/capabilities.md` for the full contract.

Passing `--evidence-dir PATH` advertises `storage.evidence.write`, making this
node an Evidence Collector: any coordinator that knows about it will forward a
copy of evidence it produces or observes, appended as JSON Lines under
`PATH/evidence-YYYYMMDD.jsonl`. `net.discovery.scan`,
`storage.evidence.write`, and `coordination.job.cancel` assess or change
external state. Scan and job control require `--execution-key`; evidence custody
requires the separate `--evidence-key`. Each must match the corresponding key on
the coordinator (Cardputer: Settings > Trust). Every request is signed and checked against
recently-seen nonces; an unsigned, wrongly-signed, or replayed request is
rejected with `UNAUTHENTICATED`. See "Authenticated capabilities" in
`docs/capabilities.md` for the exact signature scheme.

```bash
.venv-desktop-node/bin/python tools/desktop-node/reconclave_node.py \
    --enable-network-scan --execution-key "execution passphrase" \
    --evidence-dir ~/reconclave-evidence --evidence-key "evidence passphrase"
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
