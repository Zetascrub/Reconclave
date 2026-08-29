# Architecture

Reconclave separates portable domain logic from transports and hardware.

```text
Device UI / local coordinator
            |
     capability request
            |
  protocol + node registry
            |
 transport adapter (TCP, WebSocket, serial, BLE, LoRa)
            |
 remote capability handler
            |
     structured response/event
```

The first vertical slice will use the Cardputer ADV as coordinator and the
PoE-P4 as a remote node. The benign `system.info` capability will prove
discovery, announcement, capability selection, request, and response before
network assessment operations are exposed remotely.

The portable library in `common/protocol` deliberately has no Arduino,
ESP-IDF, network, JSON-library, or dynamic-runtime dependency. Transport
adapters own framing and JSON conversion; the domain layer validates the
decoded values. This lets both current devices compile the same rules and
prevents the coordinator from being the only trust boundary.

## Migration rule

Ghostwire remains a reference implementation. A feature is migrated only by:

1. defining its capability and request/result contract;
2. wrapping its device implementation in a capability handler;
3. emitting evidence with source and job provenance;
4. testing local execution first;
5. enabling remote dispatch only after authentication and scope checks exist.

This keeps device drivers reusable without importing Ghostwire's monolithic UI
state or its legacy companion API into the new protocol.
