#pragma once
#include <stdint.h>

// Which state the splash buddy shows, from the usage payload and the session
// states. Pure — no Arduino/LVGL — so it can be unit-tested on the host (see
// test/test_mascot/). main.cpp feeds it and hands the result to the splash.

// Ordered low → high priority.
enum MascotState {
    MASCOT_NONE,        // nothing to say: the usage-rate groups pick
    MASCOT_DONE,        // a session just finished
    MASCOT_WORK,        // a session is working
    MASCOT_LIMIT,       // the 5 h session limit is used up
    MASCOT_WAIT,        // a session needs you
    MASCOT_CELEBRATE,   // the limit just cleared
};

#define MASCOT_CELEBRATE_MS 10000u
#define MASCOT_DONE_MS      30000u

struct MascotInput {
    bool  usage_fresh = false;  // a usage message arrived recently (daemon alive)
    float session_pct = 0;
    bool  rejected    = false;  // the API reported the 5 h status as "rejected"
    bool  any_wait    = false;
    bool  any_work    = false;
    bool  any_done    = false;
};

// Folds one session's state ("work", "wait", "done", "idle") into `in`.
void mascot_note_state(MascotInput* in, const char* st);

// Edge memory between steps. Zero-initialised is the boot state.
struct MascotTracker {
    bool     was_limit   = false;
    bool     was_busy    = false;
    bool     celebrating = false;
    uint32_t celebrate_from = 0;
    bool     done_on     = false;
    uint32_t done_from   = 0;
};

MascotState mascot_step(MascotTracker* t, const MascotInput& in, uint32_t now_ms);

// Splash animation names for a state; NULL and *n = 0 for MASCOT_NONE.
const char* const* mascot_anims(MascotState s, int* n);

// "none", "done", "work", "limit", "wait", "celebrate" — for logs and the
// `mascot` serial command.
const char* mascot_name(MascotState s);
bool mascot_parse(const char* name, MascotState* out);
