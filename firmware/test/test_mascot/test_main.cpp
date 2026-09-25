// Host unit test for mascot_step — which state the splash buddy shows. No
// Arduino/LVGL/hardware deps, so it runs on any host:
//
//   g++ -std=c++17 -I ../../src test_main.cpp ../../src/mascot.cpp -o t && ./t

#include "mascot.h"
#include <cstdio>
#include <cstring>

static int failures = 0;
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);             \
            ++failures;                                                        \
        }                                                                      \
    } while (0)

// Fresh usage below the limit, with the given session states.
static MascotInput in(const char* a = nullptr, const char* b = nullptr) {
    MascotInput i;
    i.usage_fresh = true;
    i.session_pct = 40;
    if (a) mascot_note_state(&i, a);
    if (b) mascot_note_state(&i, b);
    return i;
}

static MascotInput at_limit(const char* a = nullptr) {
    MascotInput i = in(a);
    i.session_pct = 100;
    return i;
}

static void should_show_nothing_special_when_idle() {
    MascotTracker t;
    CHECK(mascot_step(&t, in("idle"), 0) == MASCOT_NONE);
}

static void should_work_when_any_session_works() {
    MascotTracker t;
    CHECK(mascot_step(&t, in("idle", "work"), 0) == MASCOT_WORK);
}

static void should_prefer_wait_over_work() {
    MascotTracker t;
    CHECK(mascot_step(&t, in("work", "wait"), 0) == MASCOT_WAIT);
}

static void should_sleep_at_the_limit_over_work() {
    MascotTracker t;
    CHECK(mascot_step(&t, at_limit("work"), 0) == MASCOT_LIMIT);
}

static void should_count_a_rejected_status_as_the_limit() {
    MascotTracker t;
    MascotInput i = in();
    i.session_pct = 99.4f;
    i.rejected = true;
    CHECK(mascot_step(&t, i, 0) == MASCOT_LIMIT);
}

static void should_prefer_wait_over_limit() {
    MascotTracker t;
    CHECK(mascot_step(&t, at_limit("wait"), 0) == MASCOT_WAIT);
}

static void should_celebrate_when_the_limit_clears_then_stop() {
    MascotTracker t;
    CHECK(mascot_step(&t, at_limit(), 0) == MASCOT_LIMIT);
    CHECK(mascot_step(&t, in("wait"), 1000) == MASCOT_CELEBRATE);  // beats wait
    CHECK(mascot_step(&t, in(), 1000 + MASCOT_CELEBRATE_MS - 1) == MASCOT_CELEBRATE);
    CHECK(mascot_step(&t, in(), 1000 + MASCOT_CELEBRATE_MS) == MASCOT_NONE);
}

static void should_not_celebrate_without_a_limit_before() {
    MascotTracker t;
    CHECK(mascot_step(&t, in(), 0) == MASCOT_NONE);
    MascotInput low = in();
    low.session_pct = 2;                  // an ordinary 5 h reset
    CHECK(mascot_step(&t, low, 1000) == MASCOT_NONE);
}

static void should_show_done_after_work_then_expire() {
    MascotTracker t;
    CHECK(mascot_step(&t, in("work"), 0) == MASCOT_WORK);
    CHECK(mascot_step(&t, in("done"), 500) == MASCOT_DONE);
    CHECK(mascot_step(&t, in("done"), 500 + MASCOT_DONE_MS - 1) == MASCOT_DONE);
    CHECK(mascot_step(&t, in("done"), 500 + MASCOT_DONE_MS) == MASCOT_NONE);
}

static void should_show_done_after_wait() {
    MascotTracker t;
    mascot_step(&t, in("wait"), 0);
    CHECK(mascot_step(&t, in("done"), 100) == MASCOT_DONE);
}

static void should_not_show_done_at_boot() {
    MascotTracker t;
    CHECK(mascot_step(&t, in("done"), 0) == MASCOT_NONE);
}

static void should_drop_done_when_work_resumes() {
    MascotTracker t;
    mascot_step(&t, in("work"), 0);
    mascot_step(&t, in("done"), 100);
    CHECK(mascot_step(&t, in("work"), 200) == MASCOT_WORK);
    CHECK(mascot_step(&t, in("idle"), 300) == MASCOT_NONE);   // no done row
}

static void should_ignore_sessions_when_usage_is_stale_but_keep_the_limit() {
    MascotTracker t;
    MascotInput i = in("wait");
    i.usage_fresh = false;
    CHECK(mascot_step(&t, i, 0) == MASCOT_NONE);
    MascotInput l = at_limit("wait");
    l.usage_fresh = false;
    CHECK(mascot_step(&t, l, 100) == MASCOT_LIMIT);
}

static void should_survive_millis_wraparound() {
    MascotTracker t;
    const uint32_t near_wrap = 0xFFFFFFFFu - 100;
    mascot_step(&t, in("work"), near_wrap);
    CHECK(mascot_step(&t, in("done"), near_wrap + 50) == MASCOT_DONE);
    CHECK(mascot_step(&t, in("done"), near_wrap + 50 + 1000) == MASCOT_DONE);
}

static void should_map_every_state_but_none_to_animations() {
    int n = -1;
    CHECK(mascot_anims(MASCOT_NONE, &n) == nullptr && n == 0);
    const char* const* w = mascot_anims(MASCOT_WORK, &n);
    CHECK(w != nullptr && n > 1);
    const char* const* s = mascot_anims(MASCOT_LIMIT, &n);
    CHECK(n == 1 && strcmp(s[0], "expression sleep") == 0);
}

static void should_parse_state_names_round_trip() {
    for (int s = MASCOT_NONE; s <= MASCOT_CELEBRATE; s++) {
        MascotState out;
        CHECK(mascot_parse(mascot_name((MascotState)s), &out) && out == s);
    }
    MascotState out;
    CHECK(!mascot_parse("bogus", &out));
}

int main() {
    should_show_nothing_special_when_idle();
    should_work_when_any_session_works();
    should_prefer_wait_over_work();
    should_sleep_at_the_limit_over_work();
    should_count_a_rejected_status_as_the_limit();
    should_prefer_wait_over_limit();
    should_celebrate_when_the_limit_clears_then_stop();
    should_not_celebrate_without_a_limit_before();
    should_show_done_after_work_then_expire();
    should_show_done_after_wait();
    should_not_show_done_at_boot();
    should_drop_done_when_work_resumes();
    should_ignore_sessions_when_usage_is_stale_but_keep_the_limit();
    should_survive_millis_wraparound();
    should_map_every_state_but_none_to_animations();
    should_parse_state_names_round_trip();

    if (failures) {
        printf("%d check(s) failed\n", failures);
        return 1;
    }
    printf("all mascot checks passed\n");
    return 0;
}
