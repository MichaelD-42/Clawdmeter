#pragma once
#include <stddef.h>
#include <stdint.h>

// Countdown to the reset of a used-up limit. The payload only carries whole
// minutes to the reset (rounded, refreshed every ~60 s), so the board anchors
// a target on receipt and counts the seconds down itself. Pure — no
// Arduino/LVGL — so it can be unit-tested on the host (test/test_countdown/).

enum LimitKind { LIMIT_NONE, LIMIT_SESSION, LIMIT_WEEKLY };

struct Countdown {
    bool     anchored  = false;
    uint32_t target_ms = 0;   // millis() at the reset
};

// Anchor on a payload's minutes-to-reset. A running target that is within a
// minute of the new one is kept, so the seconds don't jump back every poll.
void countdown_anchor(Countdown* c, int reset_mins, uint32_t now_ms);

// Whole seconds left, 0 once the reset is due.
uint32_t countdown_left_s(const Countdown& c, uint32_t now_ms);

// "2:14:05" under a day, "3d 04:12" beyond.
void countdown_format(uint32_t secs, char* buf, size_t n);

// Which hit limit to count down to: the one that resets later, since the
// other one clearing first doesn't free you. *mins is set unless LIMIT_NONE.
LimitKind countdown_pick(bool session_hit, int session_mins,
                         bool weekly_hit, int weekly_mins, int* mins);
