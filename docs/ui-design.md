# Reconclave interface style guide

Status: living standard
Applies to: Cardputer ADV, K230, and future embedded coordinators/nodes

Reconclave should feel like a professional cyberdeck: expressive, dark,
legible, deliberate, and useful under poor lighting. Its visual language may
borrow the energy of hacker cinema and near-future games, but atmosphere never
obscures evidence, state, scope, or safety. Device size may change, but
navigation, information priority, terminology, and colour meaning remain
consistent.

## 0. Cyberdeck identity

The interface balances two modes:

- **Instrument mode** owns working screens. Geometry is stable, copy is calm,
  and animation communicates real state.
- **Atmosphere mode** owns boot, title, idle, discovery transitions, and
  completion moments. It may be cinematic, but must remain interruptible where
  doing so is safe.

Avoid generic green-code decoration, fake warnings, meaningless percentages,
or glitches that resemble display faults. Ornament should suggest topology,
radio activity, packets, trust boundaries, and evidence flow.

The Cardputer home screen is a hybrid mission-control dashboard: a thin live
status line, one focused mission card, compact paging, and keyboard-first
navigation. It is neither a wall of status tiles nor a plain application list.

## 1. Product principles

1. Lead with the operator's task, current state, and next useful action.
2. Keep settings in the contextual `Tab` menu, not duplicated in the working view.
3. Put technical identifiers and protocol facts on detail screens.
4. Use dense lists only where comparison matters: nodes, observations, hosts,
   services, and evidence.
5. Long operations must remain responsive and show state and progress.
6. Passive observation, active assessment, and destructive actions must be
   visually and procedurally distinct.
7. Never claim that an observation is a finding without supporting evidence.

## 2. Navigation contract

These controls mean the same thing everywhere outside text entry:

| Control | Meaning |
| --- | --- |
| Up / Down | Move through a vertical list or scroll content |
| Left / Right | Browse cards or change a selected setting value |
| Enter | Open, apply, or perform the primary action |
| Tab | Open or close the current area's contextual menu |
| Q / Escape / Backspace | Return to the immediate parent |
| R | Start or refresh the primary bounded operation when shown |
| `; , . /` | Physical aliases for Up / Left / Down / Right |

Rules:

- Left is never Back.
- Back restores the parent's previous relevant selection.
- Tab never performs an action directly.
- Text-entry screens keep punctuation keys as punctuation.
- A running operation may continue after leaving only when intentional and its
  state remains visible on return.

## 3. Information hierarchy

Use this hierarchy unless a hardware constraint requires otherwise:

1. Mission home
2. Category or roster
3. Working/results view
4. Item detail or management view
5. Contextual Tab menu
6. Confirmation for disruptive or destructive actions

Examples: Reconclave → Nodes → Node management; Scout → Hosts → Host →
Services; Evidence → Files → Details → Preview.

## 4. Cardputer layout grid

The Cardputer canvas is 240 × 135 pixels. All screens reserve three regions:

| Region | Bounds | Purpose |
| --- | --- | --- |
| Header | y 0–23 | Screen title plus `NET` and `KEY` indicators |
| Content | y 24–117 | State, results, cards, lists, and progress |
| Footer | y 118–134 | Control hints plus the fixed Tab badge |

Mandatory safe areas:

- Content left/right inset: 7 px minimum.
- Header title: x 7, maximum 26 fixed-width characters.
- Header indicators begin at x 176; titles never enter that area.
- Footer hint: x 6, maximum 31 fixed-width characters.
- Tab badge: x 207–235; footer text never enters that area.
- No content baseline may be below y 108.
- Progress and result rows never share a vertical band.

Do not rely on drawing order to hide an overlap. Allocate separate geometry.

## 5. Typography and copy limits

Use the built-in fixed-width font at size 1 unless one focal value genuinely
benefits from size 2.

| Element | Maximum |
| --- | --- |
| Header title | 26 characters |
| Footer hint | 31 characters before the Tab badge |
| Full content line | 37 characters at x 8 |
| Context-menu label | 35 characters |
| Mission title at size 2 | 13 characters |
| Shared list label | 18–20 characters |

Use sentence case for guidance and uppercase only for compact state labels.
Prefer `3 hosts` to `Hosts discovered: 3`. Keep exact IDs, ports, versions, and
error codes on detail screens. Ellipsis denotes work in progress, not ordinary
truncation.

## 6. Colour system

**Canonical brand palette (single source of truth):** the actual hex values are
defined once in [`common/identity/identity.json`](../common/identity/identity.json)
and generated into `common/identity/include/reconclave/identity.h` (C++, both
`0xRRGGBB` and `RGB565`) and `common/identity/identity.css` (`--rc-*` custom
properties). Every surface consumes those rather than hardcoding hexes — C++/LVGL
and TFT devices via the header, web surfaces (desktop UI and any device web UI)
via the stylesheet. Do not copy a hex value into a device; reference the token
and, if it must change, change it in `identity.json` and regenerate.

| Token | Hex | Role |
| --- | --- | --- |
| `accent` | `#00cdd7` | Primary accent / cyan: interactive, focus, trusted, brand |
| `warning` | `#ffaa1c` | Attention / amber: running, unpaired, degraded, caution |
| `surface` | `#0e222e` | Base surface / panel background |
| `surface_raised` | `#16303f` | Raised surface: cards, headers, selected rows |
| `ink` | `#fff2d7` | Primary foreground text (warm off-white) |
| `ink_muted` | `#c9b896` | Secondary text, labels, muted detail |
| `disabled` | `#3a4552` | Unavailable / disabled capability or node |

The table below is the **semantic role layer** the on-device theme engine remaps
from (see the Cardputer `color565` remap): the RGB triples name design roles,
and each theme renders them to concrete colours. The default (Neon Grid / Field)
rendering of these roles is the canonical brand palette above.

| Role | RGB | Use |
| --- | --- | --- |
| Canvas | `5, 10, 16` | Main background |
| Chrome | `9, 28, 39` | Header and footer |
| Surface | `11, 30, 40` | Cards and panels |
| Selected surface | `22, 66, 72` | Current row, chip, or control |
| Border | `35, 118, 112` | Active card and progress outline |
| Accent | `80, 230, 190` | Ready, selected, trusted, primary state |
| Primary text | white | Main information |
| Secondary text | `150, 170, 175` | Guidance and metadata |
| Muted text | `130, 155, 160` | Unavailable or low-priority state |
| Attention | `255, 190, 70` | Running, unpaired, degraded, or caution |
| Danger | theme magenta/red | Failure, destructive, or disruptive action |

Never use pale text on a bright surface. Colour reinforces meaning but is
never the only way state is communicated.

### Theme personalities

Themes alter palette, chrome motif, animation treatment, idle presentation,
title card, and audio pitch family while preserving semantic meaning:

| Theme | Personality | Motion and chrome |
| --- | --- | --- |
| Neon Grid | Cyan/teal trusted-network deck | topology cuts, clean traces, precise motion |
| Night City | Violet, electric blue, and hot-magenta operator console | asymmetric rails, energetic sweeps, sharp cuts |
| Amber CRT | Warm phosphor field terminal | inset frames, subtle scan texture, measured motion |

Theme decoration must remain outside text safe areas. Themes may never change
navigation behavior, safety level, or the meaning of a state colour.

## 6.1 Motion and rendering

- Compose a complete frame off-screen and present it atomically whenever RAM
  permits. Never expose a sequence of clears and individual widget draws.
- Redraw only when model state, focus, or an animated region changes.
- Static screens do not run a continuous render loop.
- Working animation targets 20–30 fps; boot animation targets 30 fps.
- Prefer moving a bounded region over clearing the full screen.
- Transitions last 90–180 ms and must not delay input or network servicing.
- Flicker is an intentional rare glitch accent only; it is never a transition.
- Long operations update at a bounded cadence and preserve stable result rows.

## 6.2 Sound language

Interface audio is optional and restrained. It provides short cues for focus,
open, back, confirmation, warning, completion, and boot identity. Navigation
tones should normally remain below 30 ms; warnings may be longer but must not
loop. Themes may change pitch family, not semantic meaning. Sound volume and
enable state persist, and every cue must have a visible equivalent.

## 7. Standard components

### Header

- Title on the left; `NET`/`OFF` and `KEY`/`---` on the right.
- One-pixel divider at y 23.
- Titles are clipped, never wrapped.

### Footer

- Show only controls that work on the current screen.
- Order hints as navigation, primary action, Back.
- Keep the Tab badge visible outside provisioning/text entry.
- Do not write `Tab: menu`; the fixed badge already communicates it.

### Lists

- Up/Down wraps unless scrolling content would make wrapping unsafe.
- Use one selected surface and one `>` marker.
- Preserve selection when returning from details.
- Show four to six rows depending on secondary metadata.

### Mission cards

- Show one card at a time on Cardputer.
- Left/Right and Up/Down may browse; Enter opens.
- Include a page indicator and one plain-language description.

### Status and progress

- Lead with `READY`, `SCANNING`, `SAVED`, `OFFLINE`, or `FAILED`.
- Put the status sentence, progress bar, and results in separate rows.
- Result geometry remains stable while an operation runs.

### Contextual Tab menu

- Tab owns area-specific settings and secondary actions.
- Settings appear before actions.
- Left/Right changes values; Enter executes actions.
- Tab, Q, Escape, or Backspace closes and restores the underlying selection.
- Never duplicate a Tab setting as an editable control in the working view.
- A result may report its actual executor as evidence metadata, but must not
  resemble another settings control.

### Empty, loading, and error states

- Empty explains what is absent and the next action.
- Loading provides a stable state and measurable progress when possible.
- Error states explain what failed in plain language.
- Old success state must not remain coloured as ready after a failure.

## 8. Reconclave-specific patterns

### Node roster

- The local coordinator/node is first and labelled `THIS CARD`.
- Remote announcements are keyed by device ID and expire when stale.
- Enter opens node management; Tab contains roster-wide discovery and pairing.
- Node capabilities appear only on that node's management/context view.

### Scout

- The main screen shows state, host count, progress, and hosts only.
- Executor and service-scan scope live exclusively in Tab.
- Enter opens the selected host; service results are its child.
- Evidence records the actual executor and observation vantage.

### Observe

- Discovery lists contain observed facts only.
- Detail screens expose identifiers and radio metadata.
- Rescan, filters, capture settings, and export belong in Tab.

### Evidence

- Preserve the distinction between observation, evidence, and finding.
- Binary files may have details without preview.
- Refresh/remount belongs in Tab; Enter opens details or preview.

## 9. Active-operation safety

- Passive discovery may start with one explicit primary action.
- Active checks are bounded by default.
- Disruptive actions require a confirmation naming target, effect, and stop
  condition.
- Destructive actions use danger colour only on final confirmation.
- Never place a disruptive action beside ordinary navigation without a guard.
- Network activity is for systems the operator owns or is authorised to assess.

## 10. K230 adaptation

K230 expands this language rather than inventing another one:

- Preserve mission names, hierarchy, semantic colours, and context meaning.
- Use touch targets of at least 44 logical pixels.
- A side rail may replace footer hints, but Back and context remain distinct.
- Wider layouts may show roster and detail side by side while retaining the
  same parent/child model.
- Touch, keyboard, and hardware-button focus produce equivalent actions.

## 11. Review checklist

Before merging or flashing a UI change, verify:

- [ ] Header title ends before x 176.
- [ ] Footer hint ends before x 207.
- [ ] Content stays outside header and footer regions.
- [ ] Every string is clipped or bounded at its component limit.
- [ ] Progress, status, and results occupy separate rows.
- [ ] Direction, Enter, Tab, and Back follow the navigation contract.
- [ ] Back restores the correct parent selection.
- [ ] Tab settings are not duplicated in the working view.
- [ ] Empty, running, completed, and failed states are legible.
- [ ] Long-running work remains non-blocking.
- [ ] Passive, active, and destructive operations are distinguishable.
- [ ] The screen remains readable at the lowest supported brightness.

When a screen cannot satisfy this checklist, redesign its information hierarchy
instead of shrinking text or permitting overlap.
