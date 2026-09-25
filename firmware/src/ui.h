#pragma once
#include "data.h"
#include "ble.h"

enum screen_t {
    SCREEN_SPLASH,
    SCREEN_USAGE,
    SCREEN_SESSIONS,
    SCREEN_COUNT,
};

void ui_init(void);
void ui_update(const UsageData* data);
void ui_tick_anim(void);
void ui_show_screen(screen_t screen);
// Next (dir > 0) or previous screen: splash -> usage -> sessions, wrapping.
void ui_step_screen(int dir);
screen_t ui_get_current_screen(void);
void ui_update_ble_status(ble_state_t state, const char* name, const char* mac);
void ui_update_battery(int percent, bool charging);

// Swap the pairing hint's wording when a host keeps failing the handshake
// (ble_pairing_rejected). Without it the board shows "To pair" while the host
// shows the board as paired, and the user is left to reconcile the two.
void ui_set_pairing_rejected(bool rejected);

// ---- Hold-to-pair feedback ----
// The pairing gesture is a blind 3-second hold with a 6-second cut-off, so it
// needs to say where it is while the finger is still down. main.cpp's
// pair_tick() drives this; the overlay floats above whatever screen is up, so
// the gesture also reads on the splash.
enum pair_ui_t {
    PAIR_UI_NONE,       // nothing in progress — hide
    PAIR_UI_HOLDING,    // long-press seen, not armed yet
    PAIR_UI_ARMED,      // inside the window: releasing now pairs
    PAIR_UI_TOO_LONG,   // past the window, heading for power-off
    PAIR_UI_NOT_YET,    // gesture refused: the link hasn't been down long enough
    PAIR_UI_DONE,       // bonds cleared, advertising
};

// The overlay hides itself two seconds after the last call, so a live gesture
// has to keep reporting (pair_tick does it every loop, hold_tick every 100 ms).
// That also ends the states nothing else clears — DONE, and any gesture whose
// finger left without a release event. Repeat calls with an unchanged state are
// cheap: they only feed the timer.
void ui_set_pair_state(pair_ui_t state);

// True while an overlay owns pixels — a pairing gesture or the needs-input
// ring. The splash paints straight to the panel on PSRAM-less boards and
// checks this before its own redraws, the same way it consults
// charge_anim_is_active().
bool ui_pair_overlay_active(void);

// Claude Code's session state, from its hooks via the daemon. Replaces the
// whimsical status line with what the session is doing; "wait" (a permission
// prompt or question) blinks a ring around the panel edge over whichever
// screen is showing, until the state moves on or a tap dismisses it.
void ui_update_session(const SessionInfo* s);
// Called when Allow / Deny is tapped on a permission prompt (SessionInfo.pr_*).
void ui_set_permission_answer(void (*answer)(uint16_t id, bool allow));

// What the touch gestures do on boards with BoardCaps.touch_keys (main.cpp
// supplies the key actions). Ignored elsewhere — there every touch is a tap.
struct UiTouchKeys {
    void (*double_tap)(void);
    void (*hold_start)(void);             // finger held past LVGL's long-press time
    void (*hold_tick)(uint32_t held_ms);  // ~every 100 ms while still held (nullable)
    void (*hold_end)(uint32_t held_ms);   // that finger lifted; held_ms from first contact
};
void ui_set_touch_keys(const UiTouchKeys* keys);
