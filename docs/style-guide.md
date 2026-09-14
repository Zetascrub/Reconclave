# Reconclave product style guide

Status: canonical, living standard  
Applies to: every Reconclave screen, web surface, device display, illustration,
and future product-family interface

This document defines the common visual language. Device reviews adapt it to
their displays and controls; they do not invent a separate identity. Detailed
Cardputer interaction geometry and embedded behavior remain in
[`ui-design.md`](ui-design.md).

## 1. Identity

**Reconclave** is the product family. It should be the only product name in
titles, device identity, navigation, packaging, and user-facing documentation.

**Zeta** is the mascot. Zeta may appear in Reconclave and in unrelated projects,
so the mascot must not be described as a Reconclave product, device, module, or
sub-brand.

**Zetascrub** is the creator's account and credit name. It may appear in author,
copyright, repository-owner, or acknowledgement contexts. Never use it as the
product name or place it in product UI copy such as boot titles, themes, device
names, or feature labels.

Use this naming hierarchy:

| Context | Name |
| --- | --- |
| Product or fleet | Reconclave |
| Mascot and mascot-led default theme | Zeta |
| Specific hardware | Reconclave + device display name |
| Authorship or creator credit | Zetascrub |

The product promise is: **Plan on the desktop. Observe in the field. Keep the
evidence together.** The visual personality is curious, capable, calm under
pressure, and technically precise.

## 2. Visual direction

Reconclave blends a professional field instrument with the warmth of Zeta's
artwork:

- Working views are restrained, dark, information-led, and stable.
- Boot, splash, idle, onboarding, and completion moments may be richer and more
  illustrative.
- Cyan light, warm cream, amber detail, dark blue-black hardware, topology
  traces, and clipped technical geometry connect both modes.
- Decoration may suggest networks, radios, evidence flow, and trust boundaries.
  It must never resemble an alert, measurement, progress indicator, or control.

Avoid fake terminal noise, ornamental warning states, dense glow behind text,
continuous glitching, and generic “hacker green.” Reconclave should look like a
purpose-built tool rather than a movie prop.

## 3. Source of truth

Brand strings and canonical colour values live in
[`common/identity/identity.json`](../common/identity/identity.json). Generated
C++ and CSS tokens are the only values application code should consume. Change
the JSON, then run:

```sh
python3 tools/generate_identity.py
python3 tools/generate_identity.py --check
```

Do not paste canonical hex values into device code. A device renderer may
quantise a generated value for its colour depth, but the semantic token remains
the source.

Mascot master artwork currently lives outside this repository at
`/mnt/Storage/Coding/Misc/Mascot`. Any edited or newly generated mascot master
must be saved there first. Device-sized exports may then be checked into the
relevant device asset directory with their source and licence recorded.

## 4. Canonical Zeta theme

The **Zeta theme** is the factory default and the visual baseline used in
screenshots, documentation, and first-run experiences.

| Token | Role |
| --- | --- |
| `canvas` | Deepest page or display background |
| `surface` | Main panel and chrome background |
| `surface_raised` | Cards, menus, headers, and elevated controls |
| `surface_selected` | Selected row or focused surface |
| `border` | Active outlines, dividers, and progress tracks |
| `accent` | Brand, focus, primary action, ready, trusted, and success |
| `warning` | Running, caution, unpaired, or degraded |
| `danger` | Failure, blocked safety state, and destructive confirmation |
| `ink` | Primary text and high-value data |
| `ink_muted` | Supporting copy, labels, and metadata |
| `disabled` | Unavailable controls and stale or inactive state |

Rules:

- Colour reinforces a word, icon, shape, or position; it is never the only state
  signal.
- Accent is the default focus colour. Do not use warning or danger merely for
  visual variety.
- Danger is reserved. Destructive actions remain neutral until their final
  confirmation.
- Body text uses `ink` or `ink_muted`, not accent.
- Bright fills use dark canvas text; dark surfaces use light ink.
- Gradients, shadows, and translucent layers must be derived from the tokens
  they support.
- Data visualisations must add patterns, labels, or shapes when categories could
  otherwise be confused.

## 5. Alternate themes

Capable devices may offer alternate themes. Zeta must ship selected on a clean
flash. A user's theme choice must persist across reboot and ordinary firmware
updates whenever settings storage is retained.

Every theme is a mapping of the semantic roles in section 4, not a new component
system. It may change palette, restrained chrome motifs, idle artwork, motion
texture, and optional sound pitch family. It may not change:

- navigation, layout hierarchy, control placement, or terminology;
- the distinction between focus, warning, danger, and disabled;
- safety gates, confirmation behavior, or evidence status;
- minimum legibility, touch target size, or reduced-motion behavior.

Current personalities are:

| Theme | Character | Constraint |
| --- | --- | --- |
| Zeta | Cyan, cream, amber, warm mascot-led cyberdeck | Factory default and reference |
| Neon Grid / Field | Cooler teal network instrument | Keep state semantics identical |
| Night City | Violet, electric blue, hot-magenta console | Magenta decoration must not imitate danger |
| Amber CRT | Warm phosphor field terminal | Preserve distinct warning and danger signals |

Devices unable to store or render multiple themes use Zeta only. A theme picker
must preview the selection, apply it consistently to the whole interface, and
offer a clear path back to Zeta.

## 6. Typography and iconography

Use a two-voice type system:

- **Interface voice:** a highly legible sans serif for navigation, prose,
  actions, and touch UI. Prefer the platform/system sans on the web unless an
  approved bundled face is available.
- **Instrument voice:** a monospaced face for measurements, identifiers,
  timestamps, status labels, logs, keyboard-first hints, and compact display UI.

Use sentence case for headings and actions. Reserve uppercase for short state
labels such as `READY`, `SCANNING`, and `FAILED`. Never use the brush-lettered
mascot artwork wordmark as body text or an interface heading.

Icons use a consistent one-colour outline or compact pixel style within a given
surface. Pair unfamiliar icons with labels. Do not use emoji or mix outline,
filled, pixel, and illustrative icons in one navigation system. Zeta's circular
goggle mark or the Z glyph may serve as the compact brand mark once an approved
master asset exists.

## 7. Shape, spacing, and density

The base spacing unit is **4 px**. Prefer 4, 8, 12, 16, 24, and 32 px steps on
large displays. Embedded layouts may use physical-pixel values that preserve the
same visual rhythm.

- Working panels use restrained 0–4 px corner radii.
- Mascot and atmosphere frames may use circular forms and larger radii.
- Clipped corners are a signature accent; use them on one focal container or
  brand element, not every control.
- One-pixel rules, thin topology traces, and small corner cuts form the technical
  chrome language.
- Dense data rows stay rectangular and aligned so comparison remains easy.
- Depth comes from border contrast and subtle tonal steps before shadows or glow.

Touch targets are at least 44 × 44 logical pixels. Pointer targets are at least
32 × 32 CSS pixels. Hardware-controlled displays must keep selection and focus
obvious without relying on a hover state.

## 8. Components and states

All surfaces use the same component hierarchy:

- **Page or mission header:** product area, current task, and relevant global
  status only.
- **Navigation:** stable position and order; selected item uses accent plus shape
  or marker.
- **Panel/card:** groups one concept; raised tone and border establish hierarchy.
- **Primary action:** one dominant next action per region.
- **Status:** state word first, explanation second, useful next action last.
- **Progress:** stable geometry, real progress when measurable, and no invented
  percentages.
- **Confirmation:** names the target, effect, scope, and stop condition.

Required state treatments:

| State | Colour role | Additional signal |
| --- | --- | --- |
| Ready / trusted / complete | Accent | Explicit label or check mark |
| Running | Warning | Verb, progress, or bounded animation |
| Degraded / caution | Warning | Explanation and recovery action |
| Failed / unsafe / destructive | Danger | Explicit label and consequence |
| Disabled / unavailable | Disabled | Reduced contrast plus reason where useful |
| Selected / focused | Accent + selected surface | Border, marker, or focus ring |

Empty screens explain what is absent and what the operator can do next. Loading
screens preserve layout. Errors replace stale success styling immediately.

## 9. Mascot and logo usage

Zeta belongs in atmosphere moments: boot, splash, idle, onboarding, About, and
significant successful completion. Zeta must not appear inside routine results,
warnings, failures, evidence records, or destructive confirmations.

A compact logo may appear persistently only as part of a deliberate shell-level
brand position, such as a desktop navigation rail or large-display top bar. If a
device does not have a stable brand region, omit it. Never add a logo to an
isolated screen simply because space is available.

Consistency rules:

- Use the same approved logo treatment at the same shell location throughout a
  device UI.
- Do not redraw Zeta, alter facial features, recolour fur, mirror an asymmetric
  pose, or place UI over the face.
- Preserve transparency and aspect ratio. Do not crop through the goggles, eyes,
  muzzle, or identifying tail bands.
- Keep operational text outside detailed artwork. Use a dark scrim or dedicated
  panel rather than glow or outline effects to rescue contrast.
- Select pose by meaning: neutral/headshot for identity, scanning for discovery,
  sleeping for idle, and success pose only after a completed operation.
- Mascot animation is optional, interruptible, and disabled by reduced-motion or
  equivalent device settings.
- Alt text names Zeta and the action; decorative repeats use empty alt text.

The brush “Zetascrub” lettering in current source art is creator artwork, not the
Reconclave wordmark. Do not export it into Reconclave product chrome. Splash
adaptations must present **Reconclave** separately in interface typography.

## 10. Motion and sound

Motion describes real change. Focus transitions last 90–180 ms. Working
animation normally targets 20–30 fps; boot sequences may target 30 fps. Static
screens do not continuously redraw. Prefer bounded regional motion over full
screen effects.

Respect reduced-motion preferences on capable platforms and provide a device
setting where the platform has none. Glitches are rare atmosphere accents and
must never resemble panel faults or corrupt evidence.

Sound is optional and restrained. Focus, open, back, confirmation, warning,
completion, and boot cues may vary in pitch family by theme, but every sound has
a visible equivalent and the user's volume/enabled choice persists.

## 11. Responsive adaptation classes

Device reviews classify each surface before adapting it:

| Class | Typical surface | Adaptation |
| --- | --- | --- |
| A: expansive | Desktop/web | Multi-panel layouts, persistent navigation, optional compact logo |
| B: touch | K230-class display | 44 px targets, stacked or master/detail views, stable top-level brand region |
| C: compact controls | Cardputer | Fixed header/content/footer, keyboard-first focus, one focal card |
| D: glanceable | T-Dongle/small status display | State, identity, and one next action; Zeta chiefly at boot/idle |
| E: headless | PoE and service-only nodes | Apply the guide to web/status surfaces, logs, and setup flows only |

Reducing screen size removes simultaneous information and decoration before it
shrinks essential text or targets. Device limitations may change layout, not
terminology, semantic colour, safety meaning, or information priority.

## 12. Per-device review procedure

Review one device at a time and record:

1. Display resolution, density, colour depth, orientation, refresh constraints,
   input methods, memory budget, and persistent settings support.
2. Every visible surface: boot, idle, home, working views, menus, empty/loading/
   error states, confirmations, setup web UI, and externally visible LEDs.
3. Current token sources, hardcoded visual values, embedded artwork, typography,
   navigation, and theme persistence.
4. Gaps against this guide, ranked as safety/state semantics, legibility and
   navigation, consistency, then atmosphere polish.
5. A proposed adaptation with representative boot, home, working, and failure
   screens before implementation.
6. On-device validation at minimum brightness, under expected viewing conditions,
   with all input methods and after reboot.

The review should produce a short device conformance record containing its class,
supported themes, asset exports, intentional exceptions, screenshots or photos,
and remaining issues.

## 13. Release checklist

- [ ] Reconclave is the only product-family name; Zeta is identified as mascot.
- [ ] Zetascrub appears only as creator/account credit where relevant.
- [ ] Zeta is the factory theme and a retained user theme choice survives reboot.
- [ ] UI code consumes generated identity tokens rather than copied hex values.
- [ ] Focus, warning, danger, success, and disabled states remain distinct.
- [ ] Colour is not the only state signal.
- [ ] Text, targets, and artwork meet the device's legibility constraints.
- [ ] Logo placement is shell-level and consistent, or intentionally omitted.
- [ ] Mascot pose matches the atmosphere moment and never obscures operational UI.
- [ ] Boot and idle presentation is interruptible where safe.
- [ ] Reduced motion and sound settings persist where supported.
- [ ] Empty, loading, complete, degraded, and failed states have been reviewed.
- [ ] Device-specific exceptions are documented and tested on hardware.
