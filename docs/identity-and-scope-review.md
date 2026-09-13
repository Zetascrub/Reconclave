# Identity & scope review

Status: review checkpoint (2026-09-13)
Purpose: capture where the fleet drifted from the original idea, the decisions
taken in response, and the follow-up work those decisions imply.

This exists because the project began to feel *overwhelming and distant from
the original idea*. This document is the deliberate course-correction, so the
concern is recorded and acted against rather than re-litigated later.

## 1. What Reconclave is supposed to be

`Reconclave_Design_Document_v0.1.md` §22 is the anchor:

> Reconclave should make specialised devices cooperate rather than make every
> device pretend to be a complete penetration-testing workstation.

The value is the **conclave**: a protocol-first, capability-driven, evidence-first
fleet. Hardware is meant to be disposable; the protocol, capability model, and
evidence custody are meant to survive it.

## 2. What held (the core is intact)

- `common/protocol/` is real shared code linked by the C++/Linux devices.
- Trust model (HMAC-tagged requests, signed scopes, boot-nonce replay
  protection, encrypted evidence spools) is implemented and consistent.
- Desktop is settled as the permanent coordinator; the "K230 as flagship"
  tension is resolved in `docs/platform-roadmap.md`.
- Safety posture is deliberate and uniform: no autonomous firing, analyze-only
  defaults, post-exploitation disabled by default, scope enforced per node.

The architecture did **not** drift. What drifted was **identity** and **surface
area**.

## 3. What drifted

### 3.1 Identity fragmented at the pixel level

The style guide (`docs/ui-design.md`) defined colour *meaning* but never
canonical colour *values*, so the palette forked into three lineages:

| Where | "Cyan" | "Amber / orange" |
| :-- | :-- | :-- |
| README badges + mermaid | `#00cdd7` | `#ffaa1c` |
| K230 `ui_shell.h` + Cardputer theme | `#00cdd7` | `#ffaa1c` |
| T-Dongle-S3 `web_ui_page.h` | `#22e0f2` | `#f6a102` |

Two different brand cyans and two oranges shipped simultaneously. Each device
also re-derived the mascot assets through its own bespoke pipeline. This is the
mechanical cause of "it doesn't feel like one thing."

### 3.2 The T-Dongle-S3 was a gadget, not a node

Well-built and carefully gated, but it did not speak the Reconclave protocol
(no `common/protocol`, no `announce`, no capabilities, no evidence). It was a
standalone Rubber-Ducky-class tool wearing the mascot — the device that most
pulled the project's character toward "a collection of hacker gadgets," which
is exactly what §22 warns against.

### 3.3 Working-tree sprawl

Four `build*/` dirs, two `.venv*`, a ~1GB `.toolchains/`, a paused custom-U-Boot
investigation. Not wrong, but ambient weight that adds to the overwhelm.
Hygiene, not architecture.

## 4. "Unified firmware" — the honest reframe

A single firmware **binary** is impossible and not worth chasing: three
incompatible worlds (ESP32/Arduino, ESP-IDF/C, RISC-V Linux/glibc). What unifies
the fleet is four **layers**, not one image:

1. **Identity** — one source of truth for palette, strings, mascot. *(Was in 3+
   places; now fixed — see §5.)*
2. **Capability & protocol** — already unified for C++ devices; the gap is the
   T-Dongle. *(Decision taken — see §5.)*
3. **UX language** — `docs/ui-design.md` exists but is Cardputer-shaped and
   aspirational; needs to bind every form factor. *(Follow-up — §6.)*
4. **Shared code** — `common/protocol` is the model; identity now follows it.

## 5. Decisions taken

- **Canonical palette = the "product family" set** (`#00cdd7` / `#ffaa1c` /
  `#0e222e` / `#16303f` / `#fff2d7` / `#c9b896` / `#3a4552`), already used by the
  K230, Cardputer, and README. The T-Dongle web UI is the outlier and migrates
  onto it.
- **Single source of truth built**: `common/identity/` — `identity.json`
  (authoritative) generates `include/reconclave/identity.h` (C++, `0xRRGGBB` +
  `RGB565`) and `identity.css` (`--rc-*`) via `tools/generate_identity.py`
  (`--check` mode for CI). Wired into CMake as an interface library
  `reconclave_identity`. See `common/identity/README.md`.
- **The T-Dongle-S3 moves out into its own project** rather than becoming a
  fleet node. (This supersedes an earlier lean toward pulling it into the
  protocol.) A BadUSB / HID tool is a poor fit for a cooperating recon-and-
  evidence conclave — keeping it separate is the more faithful reading of §22.
  **The existing T-Dongle work is preserved, not deleted**; it is extracted to a
  standalone project that may share the Reconclave *brand* (via `common/identity`)
  but not the fleet protocol.

## 6. Follow-up work (not yet done)

1. **Migrate each surface onto `common/identity`** — replace the hardcoded hexes
   in `devices/k230/src/ui_shell.h`, the Cardputer theme, and the T-Dongle
   `web_ui_page.h` with the generated tokens; add a `generate_identity.py
   --check` step to CI.
2. **Extract the T-Dongle-S3 to its own project** — move `devices/t-dongle-s3/`
   out to a standalone repo/project, preserving its history and work. It may
   depend on `common/identity` for shared branding but leaves the fleet
   protocol behind. Remove it from this repo only once the extraction is
   confirmed safe.
3. **Promote `docs/ui-design.md` to bind the whole fleet** — reference the
   canonical palette values, add per-form-factor sections (Cardputer 240×135,
   K230 touch, T-Dongle 80×160, desktop web).
4. **Repo hygiene** — gitignore `build*/`, `.venv*`, `.toolchains/`.

Deliberately **not** on this list: new capabilities or new device types. The
roadmap already has 15 phases and speculative devices queued; regaining focus
means deepening the core, not widening the surface again.
