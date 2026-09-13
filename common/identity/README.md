# Reconclave identity — single source of truth

This directory is the authoritative definition of Reconclave's brand identity:
the **palette**, the **product strings**, and the **device roster**. It exists
so those facts live in exactly one place and cannot drift across the fleet the
way they did before it existed (two different brand cyans and two oranges were
shipping simultaneously).

## Authority and generation

- **[`identity.json`](identity.json) is the only file you edit.**
- The rest are **generated** — regenerate after any change:

  ```sh
  python3 tools/generate_identity.py          # rewrite the generated files
  python3 tools/generate_identity.py --check  # CI: fail if they are stale
  ```

| File | Consumed by | Form |
| :-- | :-- | :-- |
| `identity.json` | the generator; humans | authoritative source |
| `include/reconclave/identity.h` | C++/embedded (K230 LVGL, Cardputer & T-Dongle TFT_eSPI) | `0xRRGGBB` + `RGB565` constants, brand strings |
| `identity.css` | web surfaces (desktop UI, device web UIs) | `--rc-*` custom properties |

## Consuming it

**C++ / CMake** (K230, native tests):

```cmake
target_link_libraries(your_target PRIVATE reconclave_identity)
```
```cpp
#include "reconclave/identity.h"
lv_obj_set_style_bg_color(panel, lv_color_hex(reconclave::identity::kColorSurface), 0);
uint16_t accent = reconclave::identity::kColorAccent565;  // for a 16-bit panel
```

**PlatformIO** (Cardputer, T-Dongle): the devices already set
`lib_extra_dirs = ../../common`, so add `include "reconclave/identity.h"` once
this library sits alongside `protocol`.

**Web**: `@import` or link `identity.css` and reference `var(--rc-accent)` etc.

## Consumer status

Where each surface stands relative to this source of truth:

| Surface | Status |
| :-- | :-- |
| **K230 touch UI** (`devices/k230/src/ui_shell.h`) | Sources the tokens directly — its `kColor*` names alias `reconclave::identity::kColor*`. No drift possible. |
| **Desktop / device web UIs** | Reference `identity.css` (`var(--rc-*)`). |
| **Cardputer ADV theme** (`devices/cardputer-adv/src/main.cpp`) | Intentionally **not** a direct consumer. It runs a richer three-theme `color565` remap engine whose *semantic* role palette matches [`docs/ui-design.md` §6](../../docs/ui-design.md); its default (Field / Neon-Grid) theme renders those roles to values that approximate these tokens — chrome→`surface` and attention→`warning` match exactly, while accent (`#00e1eb` vs `#00cdd7`), `surface_raised`, and `ink_muted` differ by a few units as deliberate theme styling. It also defines roles this set does not (canvas, two borders, selected-surface). Considered aligned at the semantic layer; a future pass could grow this token set to the full role list and have the Cardputer remap source from it. |

## The palette decision

The canonical palette is the **"product family" set** already used by the
T-Display K230 (`devices/k230/src/ui_shell.h`), the Cardputer ADV theme, and the
top-level README badges. The T-Dongle-S3 web UI (`web_ui_page.h`) predates this
file and uses a slightly different cyan/orange; it is the one surface that must
**migrate onto these values**, not the other way round.

If you ever decide to flip the canonical hues (e.g. to the mascot-artwork
swatch), change them in `identity.json` and regenerate — every consumer follows
automatically. That is the whole point of this directory.
