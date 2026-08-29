# Unit PoE-P4 firmware

The v0.1 firmware is a wired Reconclave node. It reuses the hardware knowledge
proven by Ghostwire: IP101 Ethernet wiring and the common-anode status LED.

It advertises `_reconclave._tcp.local`, serves its announcement at
`/reconclave/v1/announce`, and accepts protocol requests at
`/reconclave/v1/message`. Only the read-only `system.info` capability is
enabled until pairing, authentication, and engagement scope exist.

The current paired MVP also exposes `net.discovery.scan` and
`coordination.job.status`. Discovery runs in a dedicated task, is bounded to
at most 254 addresses on the P4's directly attached subnet, retains at most 48
responsive hosts in RAM, and requires authenticated requests. Starting a new
scan replaces the previous in-memory result set; the Cardputer can explicitly
export completed results to microSD.

Grove UART on GPIO53/54 provides the physical trust bootstrap. The P4 accepts
its first peer record, persists it in NVS, and will only accept the same peer
secret afterward. Each boot creates a new network challenge so captured
requests cannot be replayed after a power cycle.

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
