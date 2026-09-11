// Small shared helpers for the K230 touch UI - the status bar, the
// per-screen "back to home" affordance, tile grid construction, and tile
// styling for the §18 mockup grid (Reconclave_Design_Document_v0.1.md).
// Deliberately thin: LVGL objects are cheap to build directly in each
// screen's own setup function, this header only factors out the bits
// every screen repeats.
#pragma once

#include "lvgl/lvgl.h"

namespace reconclave::ui {

// Zeta Mascot palette (devices/cardputer-adv's Display & Interface theme),
// reused here so the K230 and Cardputer read as the same product family.
constexpr uint32_t kColorPanel = 0x0e222e;
constexpr uint32_t kColorPanelLight = 0x16303f;
constexpr uint32_t kColorAccent = 0x00cdd7;
constexpr uint32_t kColorAmber = 0xffaa1c;
constexpr uint32_t kColorForeground = 0xfff2d7;
constexpr uint32_t kColorSecondary = 0xc9b896;
constexpr uint32_t kColorUnavailable = 0x3a4552;

// The RM69A10 panel has physically rounded corners - content placed flush
// against a corner gets visually clipped. Every screen keeps at least this
// much clearance from all four edges, not just the corners specifically,
// since a rotated layout means any edge can end up near a curve.
constexpr lv_coord_t kSafeMargin = 20;

// Status bar height, and the back button's, exposed so callers can
// position content below them without duplicating the values.
constexpr lv_coord_t kStatusBarHeight = 52;
constexpr lv_coord_t kBackButtonHeight = 40;
// Y offset where screen-specific content should start: below the status
// bar and back button, each separated by a small gap rather than a full
// kSafeMargin, to keep header chrome from eating too much of this
// board's modest 568px logical height in landscape.
constexpr lv_coord_t kContentTop =
    kSafeMargin + kStatusBarHeight + 8 + kBackButtonHeight + 12;

enum class TileState { Live, Unavailable };

// Builds a screen with the standard dark panel background, common to every
// screen this app shows (home and each capability screen alike).
lv_obj_t* createScreen();

// Builds a small top-left "< back" button on `screen` that switches to
// `home_screen` when tapped - every non-home screen gets exactly one of
// these, in the same place, so touch-only navigation stays predictable
// (matches Cardputer's "one rule throughout: ... Q/Escape/Backspace
// returns to parent" convention, translated to a touch affordance instead
// of physical keys since this board has none wired up yet).
void addBackButton(lv_obj_t* screen, lv_obj_t* home_screen);

// Builds the mascot logo + RECONCLAVE title + connectivity dot status bar
// strip at the top of `screen`. Content below it should start at
// y = kSafeMargin + kStatusBarHeight + kSafeMargin or later.
void addStatusBar(lv_obj_t* screen, const char* title, bool connected);

// Updates every status bar created by addStatusBar(). Values are kept in one
// place so screen changes never leave stale clock/network/battery chrome.
void updateStatusBars(const char* clock_text, const char* battery_text, bool connected);

// Builds a `cols`-per-row flex-wrap container filling the area below the
// status bar, inset by kSafeMargin on every other edge, sized so
// `cols` fixed-size tiles fit exactly one row width with even gaps
// (LVGL's LV_LAYOUT_GRID produced garbage geometry in this specific
// on-device LVGL build - confirmed via logged tile x/y/w/h coming back as
// nonsensical huge values despite LV_USE_GRID being enabled; the vendor's
// own k230_phone_ui uses lv_obj_set_flex_flow for its own layouts too, not
// grid, so this follows that same proven-working approach instead of
// chasing the grid bug further). Tiles are created via createTile() in the
// order they should appear (flex-wrap places them left-to-right,
// top-to-bottom automatically) - `rows` only affects the container's
// height, not placement.
lv_obj_t* createTileGrid(lv_obj_t* screen, int cols, int rows);

// Builds one tile inside `grid` (appended in flex order - see
// createTileGrid), styled per `state` (TileState::Unavailable renders
// greyed and non-interactive, matching the design doc's capability-aware
// principle - present, not hidden). Returns the tile so a Live caller can
// attach its own click handler.
lv_obj_t* createTile(lv_obj_t* grid, const char* label_text, TileState state,
                     const char* symbol = nullptr, const char* subtitle = nullptr);

}  // namespace reconclave::ui
