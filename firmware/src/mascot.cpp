#include "mascot.h"
#include <string.h>

void mascot_note_state(MascotInput* in, const char* st) {
    if (!st) return;
    if (strcmp(st, "wait") == 0)      in->any_wait = true;
    else if (strcmp(st, "work") == 0) in->any_work = true;
    else if (strcmp(st, "done") == 0) in->any_done = true;
}

MascotState mascot_step(MascotTracker* t, const MascotInput& in, uint32_t now_ms) {
    // The limit comes from the last usage payload even when it is stale — a
    // board that lost its daemon at 100 % is still looking at a used-up limit.
    const bool limit = in.session_pct >= 100.0f || in.rejected;
    if (t->was_limit && !limit) {
        t->celebrating = true;
        t->celebrate_from = now_ms;
    }
    t->was_limit = limit;
    if (t->celebrating && now_ms - t->celebrate_from >= MASCOT_CELEBRATE_MS)
        t->celebrating = false;

    // Session states arrive only on change, so their age says nothing; the
    // regular usage poll is what proves the daemon is still there.
    const bool wait = in.usage_fresh && in.any_wait;
    const bool work = in.usage_fresh && in.any_work;
    const bool busy = wait || work;
    if (busy) {
        t->done_on = false;
    } else if (t->was_busy && in.usage_fresh && in.any_done) {
        t->done_on = true;
        t->done_from = now_ms;
    }
    t->was_busy = busy;
    if (t->done_on && (now_ms - t->done_from >= MASCOT_DONE_MS || !in.any_done))
        t->done_on = false;

    if (t->celebrating) return MASCOT_CELEBRATE;
    if (wait)           return MASCOT_WAIT;
    if (limit)          return MASCOT_LIMIT;
    if (work)           return MASCOT_WORK;
    if (t->done_on)     return MASCOT_DONE;
    return MASCOT_NONE;
}

static const char* const ANIM_DONE[]      = { "done" };
static const char* const ANIM_WORK[]      = { "work coding", "work think", "write", "think" };
static const char* const ANIM_LIMIT[]     = { "expression sleep" };
static const char* const ANIM_WAIT[]      = { "expression surprise" };
static const char* const ANIM_CELEBRATE[] = { "dance bounce" };

#define ANIMS(a) (*n = sizeof(a) / sizeof(a[0]), a)

const char* const* mascot_anims(MascotState s, int* n) {
    switch (s) {
    case MASCOT_DONE:      return ANIMS(ANIM_DONE);
    case MASCOT_WORK:      return ANIMS(ANIM_WORK);
    case MASCOT_LIMIT:     return ANIMS(ANIM_LIMIT);
    case MASCOT_WAIT:      return ANIMS(ANIM_WAIT);
    case MASCOT_CELEBRATE: return ANIMS(ANIM_CELEBRATE);
    default:               *n = 0; return nullptr;
    }
}

static const char* const NAMES[] = { "none", "done", "work", "limit", "wait", "celebrate" };

const char* mascot_name(MascotState s) {
    return (s >= MASCOT_NONE && s <= MASCOT_CELEBRATE) ? NAMES[s] : "?";
}

bool mascot_parse(const char* name, MascotState* out) {
    for (int i = MASCOT_NONE; i <= MASCOT_CELEBRATE; i++) {
        if (strcmp(name, NAMES[i]) == 0) { *out = (MascotState)i; return true; }
    }
    return false;
}
