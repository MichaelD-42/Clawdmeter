#include <Arduino.h>
#include <Wire.h>
#include <lvgl.h>
#include <ArduinoJson.h>
#include <esp_heap_caps.h>

#include "data.h"
#include "ui.h"
#include "countdown.h"
#include "ble.h"
#include "splash.h"
#include "charge_anim.h"
#include "usage_rate.h"
#include "mascot.h"
#include "idle.h"
#include "idle_cfg.h"
#include "brightness.h"

#include "hal/board_caps.h"
#include "hal/display_hal.h"
#include "hal/touch_hal.h"
#include "hal/input_hal.h"
#include "hal/power_hal.h"
#include "hal/imu_hal.h"
#include "hal/sound_hal.h"

static UsageData usage = {};
static SessionInfo session = {};

// ---- Splash buddy (mascot.h) ----
// Session states only count while usage keeps arriving (daemon polls ~60 s);
// same window the UI uses to call its data live.
#define USAGE_FRESH_MS 90000
static MascotTracker mascot;
static uint32_t      last_usage_ms = 0;
static MascotState   mascot_shown  = MASCOT_NONE;
static bool          mascot_forced = false;          // `mascot <state>` serial command
static MascotState   mascot_forced_state = MASCOT_NONE;

// Which used-up limit the limit screen counts down to. A "rejected" status
// counts as the 5 h one, same as for the mascot.
static void push_limit() {
    int mins = 0;
    const bool session_hit = usage.session_pct >= 100.0f || strcmp(usage.status, "rejected") == 0;
    const LimitKind k = countdown_pick(session_hit, usage.session_reset_mins,
                                       usage.weekly_pct >= 100.0f, usage.weekly_reset_mins, &mins);
    ui_set_limit(k, mins);
}

// ---- LVGL draw buffers (partial render mode) ----
// PSRAM-equipped boards (S3) can comfortably hold larger strips. PSRAM-free
// boards (e.g. ESP32-C6) allocate from internal SRAM, so we shrink the strip
// — 480×20 RGB565 = 19 KB × 2 buffers = 38 KB, fits beside everything else.
#ifdef BOARD_HAS_PSRAM
#define BUF_LINES 40
#define LV_BUF_CAPS (MALLOC_CAP_SPIRAM)
#else
#define BUF_LINES 20
#define LV_BUF_CAPS (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)
#endif
static uint16_t* buf1 = nullptr;
static uint16_t* buf2 = nullptr;

// Abstecher vom Splash auf die Auslastungszahlen: alle EVERY fuer SHOW lang.
#define USAGE_PEEK_EVERY_MS  (5UL * 60UL * 1000UL)
#define USAGE_PEEK_SHOW_MS   (60UL * 1000UL)

static uint32_t my_tick(void) { return millis(); }

static void my_flush_cb(lv_display_t* disp, const lv_area_t* area, uint8_t* px_map) {
    int32_t w = area->x2 - area->x1 + 1;
    int32_t h = area->y2 - area->y1 + 1;
    display_hal_draw_bitmap(area->x1, area->y1, w, h, (uint16_t*)px_map);
    // Letzter Streifen eines Durchlaufs: der Splash wartet darauf, bevor er
    // nach einem Screenwechsel wieder direkt auf den Panel malt.
    if (lv_display_flush_is_last(disp)) splash_note_refresh_done();
    lv_display_flush_ready(disp);
}

static void rounder_cb(lv_event_t* e) {
    lv_area_t* area = (lv_area_t*)lv_event_get_param(e);
    display_hal_round_area(&area->x1, &area->y1, &area->x2, &area->y2);
}

// Touch policy is driven by IDLE_WAKE_ON_TOUCH:
//   true  → a press edge while asleep wakes the device and the first touch is
//           swallowed (mirrors the button wake-consumption); a press while
//           awake counts as activity.
//   false → touch never counts as activity and is fully swallowed while the
//           panel is dark, so pets/sleeves can't wake it overnight and LVGL
//           can't quietly toggle splash<->usage on a black panel.
static void my_touch_cb(lv_indev_t* indev, lv_indev_data_t* data) {
    uint16_t x, y;
    bool pressed;
    touch_hal_read(&x, &y, &pressed);
    const bool raw_pressed = pressed;

    if (IDLE_WAKE_ON_TOUCH) {
        static bool touch_was = false;
        static bool touch_wake_swallowed = false;
        if (raw_pressed && !touch_was) {
            // Press edge — consume as wake if asleep.
            if (idle_consume_wake_press()) {
                touch_wake_swallowed = true;
                pressed = false;
            }
        } else if (!raw_pressed && touch_was) {
            // Release edge.
            if (touch_wake_swallowed) {
                touch_wake_swallowed = false;
                pressed = false;
            }
        } else if (raw_pressed && touch_wake_swallowed) {
            // Held finger through wake — keep hiding until release.
            pressed = false;
        }
        touch_was = raw_pressed;
    } else if (idle_is_asleep()) {
        pressed = false;
    }

    if (pressed) {
        data->point.x = x;
        data->point.y = y;
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

// The daemon sends two kinds of message: the usage payload, and a small
// {"ev":1,...} one whenever Claude Code's session state changes.
enum msg_kind_t { MSG_BAD, MSG_USAGE, MSG_SESSION };

static msg_kind_t parse_json(const char* json, UsageData* out, SessionInfo* sess) {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, json);
    if (err) {
        Serial.printf("JSON parse error: %s\n", err.c_str());
        return MSG_BAD;
    }

    if (doc["ev"].is<int>()) {
        strlcpy(sess->project, doc["p"] | "", sizeof(sess->project));
        strlcpy(sess->state, doc["st"] | "idle", sizeof(sess->state));
        strlcpy(sess->step, doc["tl"] | "", sizeof(sess->step));
        strlcpy(sess->model, doc["m"] | "", sizeof(sess->model));
        sess->ctx = doc["cx"] | -1;
        sess->n_sessions = doc["sn"] | 0;
        sess->n_rows = 0;
        sess->n_agent_rows = 0;
        // "ss": [[project, state, step, ctx, [[type, objective], ...], n_agents], ...]
        for (JsonArray r : doc["ss"].as<JsonArray>()) {
            if (sess->n_rows >= SESSION_ROWS_MAX) break;
            SessionRow& row = sess->rows[sess->n_rows++];
            strlcpy(row.project, r[0] | "", sizeof(row.project));
            strlcpy(row.state, r[1] | "idle", sizeof(row.state));
            strlcpy(row.step, r[2] | "", sizeof(row.step));
            row.ctx = r[3] | -1;
            row.first_agent = sess->n_agent_rows;
            for (JsonArray a : r[4].as<JsonArray>()) {
                if (sess->n_agent_rows >= AGENT_ROWS_MAX) break;
                AgentRow& ag = sess->agents[sess->n_agent_rows++];
                strlcpy(ag.type, a[0] | "", sizeof(ag.type));
                strlcpy(ag.objective, a[1] | "", sizeof(ag.objective));
            }
            row.n_listed = sess->n_agent_rows - row.first_agent;
            row.n_agents = r[5] | row.n_listed;
        }
        // "pr": [id, project, tool, preview] — absent when nothing is asked.
        JsonArray pr = doc["pr"].as<JsonArray>();
        sess->pr_id = pr[0] | 0;
        strlcpy(sess->pr_project, pr[1] | "", sizeof(sess->pr_project));
        strlcpy(sess->pr_tool, pr[2] | "", sizeof(sess->pr_tool));
        strlcpy(sess->pr_preview, pr[3] | "", sizeof(sess->pr_preview));
        return MSG_SESSION;
    }

    out->session_pct = doc["s"] | 0.0f;
    out->session_reset_mins = doc["sr"] | -1;
    out->weekly_pct = doc["w"] | 0.0f;
    out->weekly_reset_mins = doc["wr"] | -1;
    strlcpy(out->status, doc["st"] | "unknown", sizeof(out->status));
    out->chime = doc["c"] | false;   // absent (old daemon / chime off) → stay silent
    const char* acct = doc["acct"] | "pro";
    out->enterprise = (strcmp(acct, "ent") == 0);
    out->time_pct = doc["tp"] | 0;
    out->period_days = doc["pd"] | 30;
    strlcpy(out->reset_date, doc["rd"] | "", sizeof(out->reset_date));
    strlcpy(out->anim, doc["a"] | "", sizeof(out->anim));
    out->clock_epoch = doc["t"] | 0L;
    out->clock_fmt = doc["tf"] | 24;
    strlcpy(out->user, doc["u"] | "", sizeof(out->user));
    strlcpy(out->plan, doc["pl"] | "", sizeof(out->plan));
    out->ok = doc["ok"] | false;
    out->valid = true;
    return MSG_USAGE;
}

// ---- Serial command buffer ----
#define CMD_BUF_SIZE 160   // room for a full 140-char `perm` preview
static char cmd_buf[CMD_BUF_SIZE];
static int cmd_pos = 0;

static void send_screenshot() {
#ifndef BOARD_HAS_PSRAM
    // A full RGB565 framebuffer doesn't fit in internal SRAM on PSRAM-free
    // boards (e.g. 480×480×2 = 460 KB). Capture is unsupported there.
    Serial.println("SCREENSHOT_UNSUPPORTED");
    return;
#else
    const uint32_t w = board_caps().width;
    const uint32_t h = board_caps().height;
    const uint32_t row_bytes = w * 2;
    const uint32_t buf_size = row_bytes * h;
    uint8_t* sbuf = (uint8_t*)heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM);
    if (!sbuf) {
        Serial.println("SCREENSHOT_ERR");
        return;
    }

    lv_draw_buf_t draw_buf;
    lv_draw_buf_init(&draw_buf, w, h, LV_COLOR_FORMAT_RGB565, row_bytes, sbuf, buf_size);

    lv_result_t res = lv_snapshot_take_to_draw_buf(lv_screen_active(), LV_COLOR_FORMAT_RGB565, &draw_buf);
    if (res != LV_RESULT_OK) {
        heap_caps_free(sbuf);
        Serial.println("SCREENSHOT_ERR");
        return;
    }

    Serial.printf("SCREENSHOT_START %lu %lu %lu\n",
        (unsigned long)w, (unsigned long)h, (unsigned long)buf_size);
    Serial.flush();
    Serial.write(sbuf, buf_size);
    Serial.flush();
    Serial.println();
    Serial.println("SCREENSHOT_END");
    heap_caps_free(sbuf);
#endif
}

static void show_session_demo() {
    static SessionInfo demo;
    demo = {};
    strlcpy(demo.project, "Clawdmeter", sizeof(demo.project));
    strlcpy(demo.state, "work", sizeof(demo.state));
    strlcpy(demo.step, "Editing ui.cpp", sizeof(demo.step));
    strlcpy(demo.model, "Opus 5.5", sizeof(demo.model));
    demo.ctx = 42;
    struct { const char* p; const char* st; int ctx; int agents; } rows[] = {
        { "Clawdmeter", "work", 42, 2 }, { "homelab", "wait", 67, 0 },
        { "signum-bot", "error", 12, 0 }, { "blog", "done", 88, 0 },
    };
    const char* agents[][2] = { { "Explore", "Find the HAL headers" }, { "Plan", "Design the overlay" } };
    demo.n_sessions = 4;
    for (auto& r : rows) {
        SessionRow& row = demo.rows[demo.n_rows++];
        strlcpy(row.project, r.p, sizeof(row.project));
        strlcpy(row.state, r.st, sizeof(row.state));
        row.ctx = r.ctx;
        row.first_agent = demo.n_agent_rows;
        for (int a = 0; a < r.agents; a++) {
            AgentRow& ag = demo.agents[demo.n_agent_rows++];
            strlcpy(ag.type, agents[a][0], sizeof(ag.type));
            strlcpy(ag.objective, agents[a][1], sizeof(ag.objective));
        }
        row.n_listed = row.n_agents = r.agents;
    }
    ui_update_session(&demo);
    ui_show_screen(SCREEN_SESSIONS);
}

static void check_serial_cmd() {
    while (Serial.available()) {
        char c = Serial.read();
        if (c == '\n' || c == '\r') {
            cmd_buf[cmd_pos] = '\0';
            if (strcmp(cmd_buf, "screenshot") == 0) send_screenshot();
            else if (strcmp(cmd_buf, "buzz") == 0)  sound_hal_play_reset();
            // Play the charge overlay without touching the cable — the real
            // trigger needs a USB transition, which is awkward to produce on a
            // device that is being flashed over that same cable.
            else if (strcmp(cmd_buf, "charge") == 0)   charge_anim_play(true);
            else if (strcmp(cmd_buf, "uncharge") == 0) charge_anim_play(false);
            // Fake a permission prompt for screenshots: `perm <preview>` shows
            // the approval overlay (id 0xBEEF), `perm off` takes it away.
            // Fill the sessions screen with made-up sessions for screenshots;
            // `sessdemo off` puts the real ones back.
            else if (strcmp(cmd_buf, "sessdemo") == 0)     show_session_demo();
            else if (strcmp(cmd_buf, "sessdemo off") == 0) ui_update_session(&session);
            else if (strncmp(cmd_buf, "perm ", 5) == 0) {
                const bool off = strcmp(cmd_buf + 5, "off") == 0;
                session.pr_id = off ? 0 : 0xBEEF;
                strlcpy(session.pr_project, "Clawdmeter", sizeof(session.pr_project));
                strlcpy(session.pr_tool, "Bash", sizeof(session.pr_tool));
                strlcpy(session.pr_preview, off ? "" : cmd_buf + 5, sizeof(session.pr_preview));
                ui_update_session(&session);
            }
            // Force the splash buddy into a state (none/done/work/limit/wait/
            // celebrate) for screenshots; `mascot off` hands back to the data.
            else if (strncmp(cmd_buf, "mascot ", 7) == 0) {
                MascotState st;
                if (strcmp(cmd_buf + 7, "off") == 0) mascot_forced = false;
                else if (mascot_parse(cmd_buf + 7, &st)) {
                    mascot_forced = true;
                    mascot_forced_state = st;
                }
            }
            // Fake a used-up 5 h limit with <mins> to go for screenshots; the
            // board switches to the countdown by itself. `limit off` clears it
            // (celebrate). The next real payload overrides either.
            else if (strncmp(cmd_buf, "limit ", 6) == 0) {
                const bool off = strcmp(cmd_buf + 6, "off") == 0;
                usage.session_pct = off ? 0 : 100;
                usage.session_reset_mins = off ? 0 : atoi(cmd_buf + 6);
                push_limit();
            }
            cmd_pos = 0;
        } else if (cmd_pos < CMD_BUF_SIZE - 1) {
            cmd_buf[cmd_pos++] = c;
        }
    }
}

// The splash buddy follows what Claude is doing. Only the limit and its
// release switch screens: the limit to its countdown, the release to the
// splash to celebrate. Both stay until the user switches away. The other
// states just change the animation — switching on every work/wait flip made
// the sessions screen unreadable.
// While true the splash gets no usage peek.
static bool mascot_holds_splash() {
    return mascot_shown == MASCOT_LIMIT || mascot_shown == MASCOT_CELEBRATE;
}

static void mascot_tick() {
    MascotState s = mascot_forced_state;
    if (!mascot_forced) {
        MascotInput in;
        in.usage_fresh = usage.valid && millis() - last_usage_ms < USAGE_FRESH_MS;
        in.session_pct = usage.session_pct;
        in.weekly_pct  = usage.weekly_pct;
        in.rejected    = strcmp(usage.status, "rejected") == 0;
        if (session.n_rows == 0) mascot_note_state(&in, session.state);  // older daemon
        for (int i = 0; i < session.n_rows; i++) mascot_note_state(&in, session.rows[i].state);
        s = mascot_step(&mascot, in, millis());
    }
    if (s == mascot_shown) return;
    mascot_shown = s;
    Serial.printf("mascot: %s\n", mascot_name(s));

    int n;
    const char* const* names = mascot_anims(s, &n);
    splash_set_mood(names, n);
    if (!mascot_holds_splash()) return;
    if (s == MASCOT_CELEBRATE) idle_note_activity();
    const screen_t to = s == MASCOT_LIMIT ? SCREEN_LIMIT : SCREEN_SPLASH;
    if (ui_get_current_screen() != to) ui_show_screen(to);
}

// Each board provides this. Must bring up the shared I2C bus (Wire.begin
// with the board's SDA/SCL pins) and any board-private hardware that has
// to settle before display/touch (e.g. an IO expander gating the LCD
// reset line). Called exactly once at the start of setup().
extern "C" void board_init(void);

// ---- Touch as keys (BoardCaps.touch_keys) ----
// The same three jobs the buttons do elsewhere, driven by ui.cpp's gestures:
//   double tap          → Shift+Tab, one keystroke
//   hold                → Space held until release (voice-mode PTT) — only
//                         while a host is connected, there's nobody to type to
//                         otherwise
//   hold 3-6 s, release → pairing, only once the link has been down for
//                         TOUCH_PAIR_DOWN_MS (where the hold isn't Space).
//                         Same window as the PWR gesture: 3 s to arm,
//                         disarmed past 6 s.
// The down-time guard matters: after a reflash or a radio hiccup the host
// reconnects in bursts, and a talk-hold landing in one of the gaps would
// otherwise wipe the bond — leaving the host retrying with a key the board no
// longer has. Counting from boot too, so it also covers the first seconds
// after a restart.
#define TOUCH_PAIR_MIN_MS  3000
#define TOUCH_PAIR_MAX_MS  6000
#define TOUCH_PAIR_DOWN_MS 15000

static bool     touch_space_down = false;
static uint32_t ble_down_since_ms = 0;   // last time the link went down (boot = 0)

static void touch_double_tap(void) {
    ble_keyboard_press(0x2B, 0x02);  // HID Tab + LEFT_SHIFT
    ble_keyboard_release();
}

static void touch_hold_start(void) {
    if (ble_get_state() != BLE_STATE_CONNECTED) return;
    ble_keyboard_press(0x2C, 0);     // HID Space, no mods
    touch_space_down = true;
}

// Whether this hold could pair at all — the link has to be down, and down long
// enough that a reconnect burst isn't mistaken for a dead bond.
static bool touch_pair_eligible(void) {
    return ble_get_state() != BLE_STATE_CONNECTED &&
           millis() - ble_down_since_ms >= TOUCH_PAIR_DOWN_MS;
}

// Same feedback the PWR gesture gets in pair_tick(), driven off the live hold
// instead of a button edge: on this board the gesture is a finger on the glass
// with nothing else to go by.
static void touch_hold_tick(uint32_t held_ms) {
    static bool announced = false;
    if (touch_space_down) return;          // that hold is push-to-talk, not pairing
    if (!touch_pair_eligible()) {
        // Say so rather than ignoring the hold in silence: refusing to pair
        // while the link is merely mid-reconnect is deliberate, but from the
        // outside it looks identical to a dead gesture.
        ui_set_pair_state(PAIR_UI_NOT_YET);
        return;
    }
    if (held_ms >= TOUCH_PAIR_MAX_MS) {
        ui_set_pair_state(PAIR_UI_TOO_LONG);
    } else if (held_ms >= TOUCH_PAIR_MIN_MS) {
        if (!announced) { sound_hal_play_pair_armed(); announced = true; }
        ui_set_pair_state(PAIR_UI_ARMED);
    } else {
        announced = false;
        ui_set_pair_state(PAIR_UI_HOLDING);
    }
}

static void touch_hold_end(uint32_t held_ms) {
    if (touch_space_down) {
        ble_keyboard_release();
        touch_space_down = false;
        return;
    }
    // Anything already on screen — "Release and retry", "Not yet" — stays for
    // its two seconds; only a hold too short to have meant anything is wiped
    // right away, since "Keep holding" after the finger is gone reads as a bug.
    if (held_ms < TOUCH_PAIR_MIN_MS) {
        ui_set_pair_state(PAIR_UI_NONE);
        return;
    }
    if (held_ms >= TOUCH_PAIR_MAX_MS) return;
    if (!touch_pair_eligible()) {
        Serial.println("Pair: touch hold ignored — link not down long enough");
        return;
    }
    Serial.println("Pair: touch held in window — clearing bonds, advertising");
    ble_clear_bonds();
    ui_set_pair_state(PAIR_UI_DONE);   // self-clears after a moment
    sound_hal_play_paired();
}

static const UiTouchKeys touch_keys = {touch_double_tap, touch_hold_start,
                                       touch_hold_tick, touch_hold_end};

void setup() {
    Serial.begin(115200);
    delay(300);
    Serial.println("{\"ready\":true}");

    board_init();

    display_hal_init();
    display_hal_begin();
    idle_init();        // takes over panel brightness and starts the idle timer
    brightness_init();  // load the user's saved brightness level and apply via idle

    power_hal_init();
    imu_hal_init();
    sound_hal_init();
    touch_hal_init();

    // ---- LVGL ----
    const int W = board_caps().width;
    const int H = board_caps().height;

    lv_init();
    lv_tick_set_cb(my_tick);

    buf1 = (uint16_t*)heap_caps_malloc(W * BUF_LINES * 2, LV_BUF_CAPS);
    buf2 = (uint16_t*)heap_caps_malloc(W * BUF_LINES * 2, LV_BUF_CAPS);

    lv_display_t* disp = lv_display_create(W, H);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(disp, my_flush_cb);
    lv_display_set_buffers(disp, buf1, buf2, W * BUF_LINES * 2,
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_add_event_cb(disp, rounder_cb, LV_EVENT_INVALIDATE_AREA, NULL);

    lv_indev_t* indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, my_touch_cb);

    ble_init();
    input_hal_init();

    ui_init();
    if (board_caps().touch_keys) ui_set_touch_keys(&touch_keys);
    ui_set_permission_answer(ble_send_answer);
    ui_update_ble_status(ble_get_state(), ble_get_device_name(), ble_get_mac_address());
    ui_update_battery(power_hal_battery_pct(), power_hal_is_charging());
    ui_show_screen(SCREEN_SPLASH);

    Serial.printf("Dashboard ready (%s, %dx%d), waiting for data on BLE...\n",
        board_caps().name, W, H);
}

static ble_state_t last_ble_state = BLE_STATE_INIT;

// Hold-to-pair gesture: hold the PWR button ~3s, then RELEASE → clear all BLE
// bonds and re-advertise. Clearing on *release* (not while held) is deliberate:
// holding to power the device OFF (AXP hardware shutdown at 8s) must not wipe
// the bond — a power-off hold never releases before shutdown. To stop a
// "chicken-out" release just before 8s from pairing, the gesture disarms at 6s.
//
//   ~1.5s long-press edge → PENDING
//   3.0s (+1500)          → ARMED   (release from here clears bonds)
//   6.0s (+4500)          → DISARMED (no clear; AXP powers off at 8s)
#define PAIR_ARM_AFTER_LONG_MS    1500   // 3.0s total
#define PAIR_DISARM_AFTER_LONG_MS 4500   // 6.0s total
enum pair_state_t { PAIR_IDLE, PAIR_PENDING, PAIR_ARMED };
static pair_state_t pair_state        = PAIR_IDLE;
static uint32_t     pair_long_seen_ms = 0;

static void pair_tick(void) {
    if (pair_state == PAIR_IDLE && power_hal_pwr_long_pressed()) {
        pair_state = PAIR_PENDING;
        pair_long_seen_ms = millis();
        (void)power_hal_pwr_released();  // drain any stale release edge
        ui_set_pair_state(PAIR_UI_HOLDING);
        Serial.println("PWR long-press: hold to ~3s then release to pair");
        return;
    }
    if (pair_state == PAIR_IDLE) return;

    if (power_hal_pwr_released()) {
        if (pair_state == PAIR_ARMED) {
            Serial.println("Pair: released in window — clearing bonds, advertising");
            ble_clear_bonds();
            ui_set_pair_state(PAIR_UI_DONE);      // self-clears after a moment
            sound_hal_play_paired();
        } else {
            Serial.println("Pair: released too early — cancelled");
            ui_set_pair_state(PAIR_UI_NONE);
        }
        pair_state = PAIR_IDLE;
        return;
    }

    uint32_t held = millis() - pair_long_seen_ms;
    if (pair_state == PAIR_PENDING && held >= PAIR_ARM_AFTER_LONG_MS) {
        pair_state = PAIR_ARMED;
        // The one moment that actually needs announcing: from here a release
        // pairs. Screen and speaker both say so, because the finger is on the
        // button and the eyes may not be on the panel.
        ui_set_pair_state(PAIR_UI_ARMED);
        sound_hal_play_pair_armed();
        Serial.println("Pair: armed — release to pair");
    } else if (pair_state == PAIR_ARMED && held >= PAIR_DISARM_AFTER_LONG_MS) {
        pair_state = PAIR_IDLE;  // power-off territory; don't pair
        ui_set_pair_state(PAIR_UI_TOO_LONG);
        Serial.println("Pair: disarmed (holding toward power-off)");
        return;
    }

    // Feed the overlay's watchdog for as long as the gesture is live. Without
    // this the button path would announce a state once and watch it time out
    // mid-hold — the armed window alone is three seconds long.
    ui_set_pair_state(pair_state == PAIR_ARMED ? PAIR_UI_ARMED : PAIR_UI_HOLDING);
}

void loop() {
    idle_tick();
    lv_timer_handler();
    ui_tick_anim();
    ble_tick();
    power_hal_tick();
    imu_hal_tick();
    sound_hal_tick();
    splash_tick();
    // Rotation transition (blank + ramp) would fight the idle fade — skip
    // ticks while the panel is dark. A rotation that happens during sleep
    // is detected by the next tick after wake and ramped in then.
    if (!idle_is_asleep()) display_hal_tick();

    // ---- Physical buttons ----
    //   PRIMARY   → HID Space  (Claude Code voice-mode PTT)
    //   SECONDARY → HID Shift+Tab  (mode toggle; only if the board has one)
    //   PWR       → on splash: cycle animations; on usage: cycle brightness;
    //               hold ~3s + release: pairing mode
    // First press from sleep is consumed as a wake-only event by
    // idle_consume_wake_press(); the normal action fires from the second
    // press. Activity bookkeeping happens inside idle_consume_wake_press
    // so no separate idle_note_activity() call is needed here.
    {
        static bool primary_was = false;
        static bool primary_wake_swallowed = false;
        bool primary_now = input_hal_is_held(INPUT_BTN_PRIMARY);
        if (primary_now != primary_was) {
            if (primary_now) {
                if (idle_consume_wake_press()) primary_wake_swallowed = true;
                else                            ble_keyboard_press(0x2C, 0);  // HID Space, no mods
            } else {
                if (primary_wake_swallowed) primary_wake_swallowed = false;
                else                        ble_keyboard_release();
            }
            primary_was = primary_now;
        }

        if (board_caps().button_count >= 2) {
            static bool secondary_was = false;
            static bool secondary_wake_swallowed = false;
            bool secondary_now = input_hal_is_held(INPUT_BTN_SECONDARY);
            if (secondary_now != secondary_was) {
                if (secondary_now) {
                    if (idle_consume_wake_press()) secondary_wake_swallowed = true;
                    else                            ble_keyboard_press(0x2B, 0x02);  // HID Tab + LEFT_SHIFT
                } else {
                    if (secondary_wake_swallowed) secondary_wake_swallowed = false;
                    else                          ble_keyboard_release();
                }
                secondary_was = secondary_now;
            }
        }

        if (power_hal_pwr_pressed()) {
            if (!idle_consume_wake_press()) {
                // On splash: cycle animations. Anywhere else: cycle screen
                // brightness.
                if (ui_get_current_screen() == SCREEN_SPLASH) splash_next();
                else                                          brightness_cycle();
            }
        }

        // Rotary ring: one screen per detent, both directions (splash ->
        // usage -> sessions -> splash). The first turn from sleep only wakes,
        // like a first button press.
        if (board_caps().has_encoder) {
            int steps = input_hal_encoder_steps();
            if (steps != 0 && !idle_consume_wake_press()) {
                const int dir = steps > 0 ? 1 : -1;
                for (int n = steps > 0 ? steps : -steps; n > 0; n--) ui_step_screen(dir);
            }
        }

        pair_tick();
    }

    // ---- Zwischendurch die Zahlen zeigen ----
    // Wer den Clawd als Animation laufen laesst, will trotzdem ab und zu
    // sehen wie es um die Auslastung steht. Alle USAGE_PEEK_EVERY_MS also
    // fuer USAGE_PEEK_SHOW_MS auf den Usage-Screen und wieder zurueck.
    //
    // Bewusst nur aus dem Splash heraus und nur solange der Nutzer nicht
    // selbst umschaltet: ein Tastendruck waehrend des Abstechers beendet ihn,
    // und wer laenger freiwillig auf den Zahlen steht, faengt danach mit
    // vollem Abstand wieder an - sonst wechselt das Ding vor der Nase hin
    // und her.
    {
        static uint32_t peek_ref_ms = 0;
        static bool     peeking     = false;
        const uint32_t now_ms = millis();
        const screen_t cur    = ui_get_current_screen();

        if (idle_is_asleep()) {
            peek_ref_ms = now_ms;          // im Schlaf laeuft die Uhr nicht
            peeking = false;
        } else if (peeking) {
            if (cur != SCREEN_USAGE) {     // Nutzer hat selbst umgeschaltet
                peeking = false;
                peek_ref_ms = now_ms;
            } else if (now_ms - peek_ref_ms >= USAGE_PEEK_SHOW_MS) {
                peeking = false;
                peek_ref_ms = now_ms;
                ui_show_screen(SCREEN_SPLASH);
                Serial.println("peek: zurueck zu den Animationen");
            }
        } else if (cur != SCREEN_SPLASH) {
            peek_ref_ms = now_ms;          // steht ohnehin auf den Zahlen
        } else if (mascot_holds_splash()) {
            peek_ref_ms = now_ms;          // Limit: der Clawd schlaeft, bleibt stehen
        } else if (now_ms - peek_ref_ms >= USAGE_PEEK_EVERY_MS) {
            peeking = true;
            peek_ref_ms = now_ms;
            ui_show_screen(SCREEN_USAGE);
            Serial.println("peek: zeige kurz die Auslastung");
        }
    }

    ble_state_t bs = ble_get_state();
    if (bs != last_ble_state) {
        if (last_ble_state == BLE_STATE_CONNECTED) ble_down_since_ms = millis();
        last_ble_state = bs;
        ui_update_ble_status(bs, ble_get_device_name(), ble_get_mac_address());
    }

    // Polled rather than event-driven: this goes true on a failed handshake but
    // false again by simply timing out, which no BLE event announces. The UI
    // ignores repeats.
    ui_set_pairing_rejected(ble_pairing_rejected());

    static int  last_pct      = -2;
    static bool last_charging = false;
    int  pct      = power_hal_battery_pct();
    bool charging = power_hal_is_charging();
    if (pct != last_pct || charging != last_charging) {
        if (pct != last_pct) ble_set_battery_level(pct);
        last_pct = pct;
        last_charging = charging;
        ui_update_battery(pct, charging);
    }

    // Cable in / out gets a short animation. Driven by VBUS rather than the
    // charging flag, which also drops when the battery reaches full with the
    // cable still in — that would play the unplug sequence for nothing.
    // The first reading only records the state: booting on USB is not an event.
    static bool vbus_known = false;
    static bool last_vbus  = false;
    bool vbus = power_hal_is_vbus_in();
    if (!vbus_known) {
        vbus_known = true;
        last_vbus  = vbus;
    } else if (vbus != last_vbus) {
        last_vbus = vbus;
        Serial.printf("USB %s\n", vbus ? "in" : "out");
        charge_anim_play(vbus);
    }

    check_serial_cmd();

    if (ble_has_data()) {
        msg_kind_t kind = parse_json(ble_get_data(), &usage, &session);
        if (kind == MSG_SESSION) {
            // Claude is stuck on a question: light the panel up so the alert
            // is seen, not just drawn on a dark screen, and knock once when
            // it starts waiting (not on every repeat of the same state). A
            // permission prompt for the board counts from the moment it
            // arrives — the "wait" notification only follows ~6 s later.
            static bool was_waiting = false;
            const bool waiting = strcmp(session.state, "wait") == 0 || session.pr_id;
            if (waiting) idle_note_activity();
            if (waiting && !was_waiting) sound_hal_play_attention();
            was_waiting = waiting;
            ui_update_session(&session);
            ble_send_ack();
        } else if (kind == MSG_USAGE) {
            last_usage_ms = millis();
            int g_before = usage_rate_group();
            bool session_reset = usage_rate_sample(usage.session_pct);
            int g_after = usage_rate_group();
            // 5-hour session limit refilled → chime so the user knows they can
            // use Claude again (no-op on boards without a buzzer). Gated on the
            // daemon's opt-in `chime` config; the `buzz` serial cmd ignores it.
            if (session_reset && usage.chime) {
                Serial.println("session reset detected — chime");
                sound_hal_play_reset();
            }
            // Host-driven animation. Sent only when the host is configured to
            // mirror its desktop buddy; absent → "" → device keeps deciding.
            splash_set_anim(usage.anim);
            if (g_after != g_before) {
                Serial.printf("usage rate: group %d -> %d (s=%.2f%%)\n",
                    g_before, g_after, usage.session_pct);
                if (splash_is_active()) splash_pick_for_current_rate();
            }
            ui_update(&usage);
            push_limit();
            ble_send_ack();
        } else {
            ble_send_nack();
        }
    }

    mascot_tick();

    delay(5);
}
