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

## Desktop coordinator

The desktop runtime is a protocol participant and local application server. Its
Python process owns mDNS, node expiry, authenticated request signing, and LAN
traffic. The React interface receives roster changes over a server-sent event
stream and sends operator actions to loopback-only JSON endpoints. Browsers do
not discover nodes, hold trust-domain keys, or contact assessment nodes
directly.

The public LAN listener exposes only the Reconclave announcement and message
endpoints. Coordinator APIs reject non-loopback clients. This permits the same
process to act as a node, coordinator, or both without turning the browser UI
into a remote control surface.

Assessment dispatch has layered scope enforcement. The web workflow requires
an operator acknowledgement, the desktop API bounds discovery to a coherent
IPv4 `/24` or smaller, and the provider independently checks its local scope.
The acknowledgement is UI/API state and is never presented to a provider as
authorization in place of a signed engagement scope.

## Migration rule

Ghostwire remains a reference implementation. A feature is migrated only by:

1. defining its capability and request/result contract;
2. wrapping its device implementation in a capability handler;
3. emitting evidence with source and job provenance;
4. testing local execution first;
5. enabling remote dispatch only after authentication and scope checks exist.

This keeps device drivers reusable without importing Ghostwire's monolithic UI
state or its legacy companion API into the new protocol.
