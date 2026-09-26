#include "countdown.h"
#include <stdio.h>

#define REANCHOR_MS 60000

void countdown_anchor(Countdown* c, int reset_mins, uint32_t now_ms) {
    if (reset_mins < 0) reset_mins = 0;
    const uint32_t target = now_ms + (uint32_t)reset_mins * 60000u;
    const int32_t  drift  = (int32_t)(target - c->target_ms);
    if (c->anchored && drift <= REANCHOR_MS && drift >= -REANCHOR_MS) return;
    c->target_ms = target;
    c->anchored  = true;
}

uint32_t countdown_left_s(const Countdown& c, uint32_t now_ms) {
    const int32_t left = (int32_t)(c.target_ms - now_ms);
    return left > 0 ? (uint32_t)left / 1000u : 0;
}

void countdown_format(uint32_t secs, char* buf, size_t n) {
    const uint32_t d = secs / 86400, h = secs / 3600 % 24, m = secs / 60 % 60;
    if (d) snprintf(buf, n, "%lud %02lu:%02lu", (unsigned long)d, (unsigned long)h, (unsigned long)m);
    else   snprintf(buf, n, "%lu:%02lu:%02lu", (unsigned long)(secs / 3600), (unsigned long)m,
                    (unsigned long)(secs % 60));
}

LimitKind countdown_pick(bool session_hit, int session_mins,
                         bool weekly_hit, int weekly_mins, int* mins) {
    if (weekly_hit && (!session_hit || weekly_mins >= session_mins)) {
        *mins = weekly_mins;
        return LIMIT_WEEKLY;
    }
    if (session_hit) {
        *mins = session_mins;
        return LIMIT_SESSION;
    }
    return LIMIT_NONE;
}
