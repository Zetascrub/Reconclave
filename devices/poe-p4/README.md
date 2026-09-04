# Unit PoE-P4 firmware

The v0.1 firmware is a wired Reconclave node. It reuses the hardware knowledge
proven by Ghostwire: IP101 Ethernet wiring and the common-anode status LED.

It advertises `_reconclave._tcp.local`, serves its announcement at
`/reconclave/v1/announce`, and accepts protocol requests at
`/reconclave/v1/message`. Trusted capabilities are enabled only when the
generated fleet provisioning header is present at build time.

The current paired firmware exposes `net.discovery.scan`,
`coordination.job.status`, authenticated `coordination.job.cancel`, and
`net.connectivity.check`, and a passive `net.arp.snapshot`. Discovery runs in a dedicated task, is bounded to
at most 254 addresses on the P4's directly attached subnet, retains at most 48
responsive hosts in RAM, and requires authenticated requests. Starting a new
scan replaces the previous in-memory result set; the Cardputer can explicitly
export completed results to microSD.

Discovery also supports non-overlapping volatile recurring runs from 10 seconds
to 24 hours. The job retains its ID, reports `run_count`, waits the requested
interval after each completed pass, and can be stopped idempotently. Connectivity
checks report Ethernet, DHCP, gateway reachability, DNS resolution, and a
conservative `internet_possible` signal without executing arbitrary code.

Up to four automation rules are stored in the P4's NVS. Each rule includes its
project ID, condition, built-in playbook, enabled state, and optional recurring
interval, so it survives loss of the coordinator and cold boots. Autonomous
results enter a three-record NVS outbox. A trusted collector can read and
acknowledge those records later; deterministic record IDs make repeated reads
safe during interrupted synchronisation.

The current fleet build embeds a unique key for each authorised coordinator.
It authenticates the desktop primary at priority 100 and Cardputer secondary at
priority 50; a signed, expiring lease prevents concurrent ownership and allows
deterministic failover. Each boot creates a new network challenge so captured
requests cannot be replayed after a power cycle. Grove UART on GPIO53/54 and
its legacy NVS record remain a recovery/development path, not the authoritative
network command trust source for a provisioned build.

## Build

Activate ESP-IDF 5.4.2, then run:

```sh
cd devices/poe-p4
idf.py set-target esp32p4
idf.py build
```

## Flash

Confirm the serial port before writing, then use:

```sh
idf.py -p /dev/ttyACM0 flash monitor
```

The common-anode LED reports red when Ethernet has not started, blue while
waiting for link, amber while waiting for DHCP, and green when the node has an
address.
