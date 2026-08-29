# Reconclave --- Distributed Security Assessment Platform

**Design Document:** v0.1\
**Status:** Initial architecture\
**Project type:** Open-source distributed reconnaissance and assessment
platform\
**Origin:** Successor to Ghostwire\
**Primary initial targets:** LILYGO T-Display K230 Kit, M5Stack
Cardputer ADV, M5Stack Unit PoE-P4\
**Optional analysis tier:** Local laptop/server running Ollama,
llama.cpp, or another explicitly approved AI provider

------------------------------------------------------------------------

## 1. Executive Summary

Reconclave is a distributed security assessment platform designed to
allow heterogeneous embedded devices to discover one another, advertise
capabilities, coordinate authorised reconnaissance tasks, collect
structured evidence, and optionally submit that evidence to a local AI
system for deeper analysis.

The project evolves Ghostwire from a standalone embedded reconnaissance
tool into a capability-driven platform.

Reconclave is not intended to make an LLM autonomously conduct a
penetration test. The core workflow should remain deterministic and
reproducible:

1.  Discover.
2.  Enumerate.
3.  Run appropriate checks.
4.  Collect evidence.
5.  Correlate observations.
6.  Present candidate findings and recommended follow-up actions.

AI is used where it provides a genuine advantage. The K230 provides
edge-AI functions such as vision, OCR, image recognition, and
classification. More computationally demanding reasoning is delegated to
a local laptop or server.

The defining component is the **Reconclave Protocol**, providing a
common communication and capability model between participating nodes.

------------------------------------------------------------------------

## 2. Project Principles

-   **Distributed by design.** No single hardware platform defines
    Reconclave.
-   **Capability driven.** Nodes advertise what they can do rather than
    the coordinator assuming functionality from device type.
-   **Evidence first.** Facts and captured evidence remain separate from
    interpretation.
-   **Deterministic assessment.** Scanning and validation primarily use
    conventional tools, parsers, and rules.
-   **AI assisted, not AI trusted.** AI may classify, correlate,
    summarise, and recommend, but should not silently establish
    findings.
-   **Privacy conscious.** Client data remains local unless an
    engagement explicitly permits an external provider.
-   **Transport independent.** Reconclave messages can use multiple
    communication methods.
-   **Open and extensible.** Third-party hardware can eventually
    implement the protocol.
-   **Scope aware.** Assessment operations remain constrained to
    explicitly authorised targets.
-   **Hardware specialised.** Devices contribute what they are good at
    rather than every node pretending to be a complete pentesting
    workstation.

------------------------------------------------------------------------

## 3. High-Level Architecture

``` text
                       RECONCLAVE
                 Distributed Assessment
                         Platform

                            |
                   Reconclave Protocol
                            |
          +-----------------+-----------------+
          |                 |                 |
          v                 v                 v
   T-Display K230    Cardputer ADV        PoE-P4
   --------------    -------------        ------
   Touch UI          Keyboard             Ethernet
   Coordinator       GPS                  PoE
   Camera            LoRa                 USB Host
   Edge AI           BLE                  Network Tasks
   Vision/OCR        Mobile Terminal      Wired Visibility
          |                 |                 |
          +-----------------+-----------------+
                            |
                            v
                    Evidence Store
                            |
                            v
                   Local AI Server
                  -----------------
                  Ollama / llama.cpp
                  Specialist models
                  Analysis / correlation
```

The K230 is expected to become the primary interactive console, but is
not a mandatory permanent master. Capable nodes may communicate directly
where appropriate.

------------------------------------------------------------------------

## 4. Device Roles

### 4.1 LILYGO T-Display K230

The K230 is the intended flagship Reconclave interface.

Potential responsibilities:

-   Touchscreen UI
-   Node discovery and management
-   Job orchestration
-   Assessment status
-   Evidence review
-   Camera input
-   OCR
-   Object detection
-   Image classification
-   QR/barcode recognition
-   Small specialised edge-AI inference
-   Wi-Fi/BLE communication
-   Local evidence caching

The K230 should primarily be treated as an **edge perception and
orchestration device**, not an Ollama box.

### 4.2 M5Stack Cardputer ADV

Potential responsibilities:

-   Physical keyboard
-   GPS
-   BLE scanning
-   LoRa communications
-   Wi-Fi reconnaissance
-   Portable terminal
-   Independent Reconclave controller
-   Field notes and interaction

Useful Ghostwire functionality can initially be migrated into this
target.

### 4.3 M5Stack Unit PoE-P4

Potential responsibilities:

-   Ethernet connectivity
-   PoE-powered unattended operation
-   Network discovery
-   ARP enumeration
-   ICMP checks
-   TCP connectivity
-   HTTP probing
-   USB host functionality
-   Wired network visibility

The P4 is particularly suitable for distributed scanning and providing a
different network vantage point from wireless nodes.

### 4.4 Laptop / Local Server

Potential responsibilities:

-   Heavy reconnaissance tooling
-   Evidence storage/database
-   Local LLM inference
-   RAG over security documentation and methodology
-   Cross-node correlation
-   Finding prioritisation
-   Suggested follow-up checks
-   Report-writing assistance

The laptop should not be mandatory for basic Reconclave operation.

------------------------------------------------------------------------

## 5. Reconclave Protocol

Initial protocol identifier:

``` text
reconclave/1
```

Firmware and protocol versions should be independent.

### 5.1 Core Message Types

  Type         Purpose
  ------------ -------------------------------------------------
  `announce`   Advertise node identity and capabilities
  `request`    Request an action from another node
  `response`   Return acknowledgement or requested information
  `event`      Send asynchronous status, evidence, or progress
  `stream`     Transfer continuous or chunked output

### 5.2 Example Announcement

``` json
{
  "proto": "reconclave/1",
  "type": "announce",
  "device_id": "rc-p4-01",
  "device_type": "poe-p4",
  "firmware": "0.1.0",
  "capabilities": [
    "net.arp.scan",
    "net.icmp.ping",
    "net.tcp.connect",
    "net.http.probe"
  ]
}
```

### 5.3 Capability Namespaces

``` text
net.discovery.scan
net.arp.scan
net.icmp.ping
net.tcp.connect
net.http.probe
net.tls.inspect

radio.ble.scan
radio.lora.send
radio.lora.receive

location.gps.read
location.gps.stream

input.keyboard
input.touch

vision.camera.capture
vision.object.detect
vision.image.classify
vision.ocr

ai.observation.classify
ai.change.detect
ai.task.suggest

storage.file.read
storage.file.write
```

The coordinator should build functionality dynamically from currently
available capabilities.

------------------------------------------------------------------------

## 6. Transport Layer

The logical Reconclave Protocol should not depend on a single transport.

Potential transports:

-   Ethernet
-   Wi-Fi
-   TCP
-   WebSocket
-   BLE
-   USB CDC / serial
-   LoRa for constrained messages

Initial development should favour easy debugging. JSON is suitable for
the first implementation.

Later versions may introduce CBOR, MessagePack, or binary framing for
constrained links or higher-throughput operations.

------------------------------------------------------------------------

## 7. Node Discovery

Reconclave nodes should discover compatible nodes automatically where
the transport permits it.

Nodes advertise:

-   Device ID
-   Device type
-   Firmware version
-   Reconclave Protocol version
-   Capabilities
-   Current status
-   Optional performance information
-   Optional transport information

Node disappearance should not destabilise the platform. Capabilities
provided exclusively by the lost node simply become unavailable.

------------------------------------------------------------------------

## 8. Job System

Long-running operations should use asynchronous jobs.

``` text
Request
   |
   v
Job accepted
   |
   v
Job ID returned
   |
   v
Progress / evidence events
   |
   v
Complete / Failed / Cancelled
```

Example:

``` text
K230 -> P4: Start net.discovery.scan
P4 -> K230: Accepted, job_id=scan-1842
P4 -> K230: Host 192.168.8.14 discovered
P4 -> K230: Progress 48%
P4 -> K230: Complete
```

This allows multiple nodes to operate concurrently without blocking the
interface.

------------------------------------------------------------------------

## 9. Distributed Scanning

Distributed scanning is a first-class Reconclave use case.

Given an authorised scope such as:

``` text
192.168.8.0/24
```

Reconclave can distribute discovery across multiple capable nodes.

### 9.1 Parallel Mode

The scope is divided into small work units rather than assigning an
entire half-subnet to a single node.

Example:

``` text
192.168.8.0/28
192.168.8.16/28
192.168.8.32/28
...
192.168.8.240/28
```

Nodes repeatedly request work, scan the assigned chunk, stream evidence,
complete the work unit, and request another.

Benefits:

-   Faster completion
-   Dynamic load balancing
-   Easy redistribution when nodes disconnect
-   Reduced impact from slow nodes

### 9.2 Consensus Mode

Multiple nodes independently perform equivalent checks against the same
authorised targets.

Example:

``` text
192.168.8.73

RC-P4-01       DETECTED
RC-K230-01     NOT OBSERVED

Concord: LOW
Action: investigate discrepancy
```

Consensus mode can expose:

-   Different network visibility
-   Segmentation behaviour
-   Routing differences
-   Firewall behaviour
-   Wireless versus wired visibility
-   Intermittent hosts

Reconclave must distinguish **not observed** from an explicit negative
result.

### 9.3 Adaptive Mode

Adaptive scheduling should eventually become the preferred distributed
mode.

Reconclave measures node performance and assigns work according to
capability and throughput.

Faster nodes receive more work. Specialised nodes receive only jobs they
can perform.

------------------------------------------------------------------------

## 10. Evidence Model

Evidence must be stored separately from interpretation.

Example:

``` json
{
  "job_id": "scan-1842",
  "source_node": "rc-p4-01",
  "target": "192.168.8.25",
  "timestamp": "2026-08-25T22:30:00+01:00",
  "observation": {
    "service": "https",
    "port": 443,
    "tls_versions": ["1.0", "1.2"],
    "hsts": false
  }
}
```

Every observation should retain:

-   Source node
-   Timestamp
-   Job
-   Target
-   Collection method
-   Relevant raw evidence
-   Parsed result
-   Confidence where applicable

Conflicting evidence must never be silently discarded.

------------------------------------------------------------------------

## 11. Edge AI

The K230 NPU should primarily provide **perception and classification**,
not general-purpose LLM inference.

Suitable functions include:

-   OCR
-   Object detection
-   Image classification
-   QR/barcode recognition
-   Gesture detection
-   Scene/change detection
-   Small specialised classifiers

### 11.1 Self-Documenting Evidence

``` text
Camera / sensor
      |
      v
Edge AI
      |
      v
Recognition / OCR
      |
      v
Structured observation
      |
      v
Evidence storage
      |
      v
Local AI server
```

Example structured observation:

``` json
{
  "type": "physical_observation",
  "source": "rc-k230-01",
  "location": "Reception",
  "observations": [
    "access_control_reader",
    "printed_wifi_notice"
  ],
  "ocr_text": "Guest Wi-Fi: ACME-GUEST",
  "confidence": 0.91
}
```

The K230 may generate deterministic or templated notes. Rich
interpretation belongs on the larger AI tier.

------------------------------------------------------------------------

## 12. Local AI Analysis

Reconclave should support a pluggable AI analysis layer.

``` text
Reconclave AI Gateway
        |
        +-- Ollama
        +-- llama.cpp
        +-- Local OpenAI-compatible API
        +-- Approved external provider
```

### 12.1 AI Responsibilities

The analysis model may:

-   Correlate observations
-   Prioritise evidence
-   Explain potential significance
-   Recommend additional checks
-   Identify missing evidence
-   Summarise assessment activity
-   Assist with technical finding descriptions
-   Assist with report writing

The model should not be the authoritative source of whether a
vulnerability exists.

### 12.2 RAG and Specialist Knowledge

Local models can be improved through retrieval over trusted security
material:

-   Reconclave methodology
-   Internal testing procedures
-   OWASP guidance
-   Vendor documentation
-   Vulnerability references
-   Sanitised finding templates
-   Command references
-   Assessment checklists

The goal is to make the **platform knowledgeable about security**,
rather than relying entirely on knowledge embedded in the LLM.

------------------------------------------------------------------------

## 13. AI Privacy Policy

AI processing should be controlled per engagement.

Example local-only policy:

``` yaml
ai_policy:
  external_processing: false
  local_models: true
  retain_evidence: true
```

An engagement explicitly permitting an approved external provider could
use:

``` yaml
ai_policy:
  external_processing: true
  provider: approved-provider
  allowed_data:
    - structured_findings
    - sanitised_headers
  prohibited_data:
    - credentials
    - customer_files
    - screenshots
    - personal_data
```

The software should enforce these policies rather than relying solely on
operator memory.

------------------------------------------------------------------------

## 14. Security Model

Because Reconclave nodes can request actions from one another, protocol
security must be designed in from the beginning.

Required controls should include:

-   Trusted-node pairing
-   Node authentication
-   Message integrity
-   Optional encryption
-   Session keys
-   Replay protection
-   Capability-level permissions
-   Explicit authorised scope
-   Audit logging
-   Firmware/update operations separated from normal assessment
    capabilities
-   Protocol-version validation

### 14.1 Scope Enforcement

Scanning nodes must independently validate requested targets against
engagement scope.

The coordinator should not be the sole enforcement point.

Example:

``` yaml
scope:
  networks:
    - 192.168.8.0/24
  prohibited:
    - 192.168.8.1
```

A node receiving an invalid job should reject it.

------------------------------------------------------------------------

## 15. Repository Strategy

Reconclave should use a monorepo.

``` text
reconclave/
├── README.md
├── docs/
│   ├── architecture.md
│   ├── protocol.md
│   ├── capabilities.md
│   └── security.md
├── protocol/
│   ├── schemas/
│   ├── message-types/
│   └── test-vectors/
├── common/
│   ├── crypto/
│   ├── serialization/
│   ├── device-model/
│   └── utilities/
├── devices/
│   ├── k230/
│   ├── cardputer-adv/
│   └── poe-p4/
├── server/
│   ├── api/
│   ├── orchestrator/
│   ├── evidence/
│   ├── ai/
│   └── storage/
├── tools/
│   ├── protocol-debugger/
│   └── provisioning/
└── .github/
    └── workflows/
```

Each hardware target may use its own SDK and build system. A monorepo
does not require one compiler.

------------------------------------------------------------------------

## 16. Ghostwire Migration

Reconclave should begin as a successor to Ghostwire rather than
discarding useful code.

``` text
Ghostwire
    |
    +-- Preserve original project
    |
    +-- Fork / clone useful code
             |
             v
         Reconclave
             |
             v
     Separate hardware code
             |
             v
     Extract capabilities
             |
             v
     Introduce protocol
             |
             v
     Add distributed jobs
             |
             v
     Add additional nodes
```

Existing Ghostwire functions should gradually move behind Reconclave
capability interfaces.

For example:

``` text
Old:

UI
 |
 v
scanNetwork()
```

becomes:

``` text
New:

UI
 |
 v
net.discovery.scan request
 |
 v
Scheduler
 |
 v
Suitable Reconclave node
 |
 v
Scan implementation
 |
 v
Structured evidence
```

Initially the requesting device and executing node may be the same
device. This proves the abstraction before distributed execution is
introduced.

------------------------------------------------------------------------

## 17. Build and Release Strategy

Reconclave should produce independent build artifacts.

Example:

``` text
Reconclave Platform 0.4.0
Reconclave Protocol 1.1

reconclave-k230-v0.4.0
reconclave-cardputer-adv-v0.4.0
reconclave-p4-v0.4.0
reconclave-server-v0.4.0
```

CI should eventually:

1.  Validate protocol schemas.
2.  Run protocol test vectors.
3.  Build supported embedded targets.
4.  Test server components.
5.  Run compatibility tests.
6.  Package release artifacts.

------------------------------------------------------------------------

## 18. K230 Interface Concept

``` text
+------------------------------+
| RECONCLAVE          87%   ●  |
+------------------------------+
|                              |
|  NETWORK       WIRELESS      |
|                              |
|  RECON         VISION        |
|                              |
|  EVIDENCE      DEVICES       |
|                              |
+------------------------------+
| K230 ●   ADV ●   P4 ●        |
+------------------------------+
```

The interface should be capability-aware.

If the P4 joins, wired-network capabilities appear. If the Cardputer ADV
joins, GPS/LoRa/keyboard capabilities appear. If a node disconnects, its
capabilities become unavailable without affecting unrelated functions.

------------------------------------------------------------------------

## 19. Development Roadmap

### v0.1 --- Protocol Proof

Goal: prove the basic architecture.

-   Create the Reconclave repository from useful Ghostwire foundations.
-   Define protocol envelope.
-   Define node identity.
-   Implement `announce`.
-   Implement `request`.
-   Implement `response`.
-   Implement basic `event`.
-   Define the initial capability namespace.
-   Cardputer ADV advertises capabilities.
-   P4 advertises capabilities.
-   K230 or Cardputer discovers nodes.
-   Display connected devices.
-   Execute one benign remote information request.

Success condition:

``` text
discover
   |
   v
advertise capabilities
   |
   v
request
   |
   v
execute remotely
   |
   v
response
   |
   v
display result
```

### v0.2 --- Jobs and Evidence

-   Asynchronous job model
-   Job IDs
-   Progress events
-   Cancellation
-   Timeouts
-   Evidence schema
-   Evidence provenance
-   Local evidence storage
-   Initial node authentication

### v0.3 --- Distributed Discovery

-   Engagement scope definition
-   Scope enforcement
-   Small work-unit queue
-   Parallel discovery
-   Result streaming
-   Result merging
-   Node disconnect handling
-   Automatic work requeue

### v0.4 --- Consensus and Adaptive Scheduling

-   Consensus scanning
-   Evidence comparison
-   Discrepancy reporting
-   Node performance measurement
-   Adaptive work allocation
-   Capability-aware scheduling

### v0.5 --- AI Analysis

-   Local AI server API
-   Ollama integration
-   Structured evidence submission
-   RAG knowledge-source support
-   Analysis responses
-   Recommended next actions
-   Human confirmation workflow

### Future

-   K230 OCR workflows
-   K230 object detection
-   Physical-assessment evidence capture
-   LoRa transport
-   Additional hardware nodes
-   Segmentation/topology analysis
-   Desktop/web interface
-   Third-party Reconclave SDK
-   Compact binary protocol formats where beneficial

------------------------------------------------------------------------

## 20. Initial Success Criteria

Reconclave's architecture is considered proven when:

1.  At least two different hardware platforms communicate using the
    Reconclave Protocol.
2.  Nodes dynamically advertise capabilities.
3.  A coordinator requests a capability without requiring
    hardware-specific knowledge.
4.  A remote node executes a job and streams results.
5.  Evidence retains node and job provenance.
6.  Work can be redistributed when a node disappears.
7.  Additional hardware can implement the same protocol without
    redesigning the core architecture.

------------------------------------------------------------------------

## 21. Project Identity

**Name:** Reconclave\
**Meaning:** Reconnaissance + Conclave

The name represents independent reconnaissance nodes coming together to
pool capabilities and evidence.

The project can use a mascot for a more playful open-source identity
without forcing mascot terminology into the technical architecture. A
meerkat or raccoon fits the themes of curiosity, scouting, cooperation,
and environmental awareness.

Technical terminology remains straightforward:

``` text
Reconclave Platform
Reconclave Protocol
Reconclave Node
Reconclave Coordinator
Reconclave Agent
Reconclave Server
Reconclave Capability
Reconclave Job
Reconclave Evidence
```

------------------------------------------------------------------------

## 22. Guiding Principle

> **Reconclave should make specialised devices cooperate rather than
> make every device pretend to be a complete penetration-testing
> workstation.**

The protocol, capability model, evidence format, and scheduler are
therefore more important than any individual scanner, device, or user
interface.

Hardware will change.

Reconclave should survive it.
