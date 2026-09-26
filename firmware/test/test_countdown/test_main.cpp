// Host unit test for the limit countdown. No Arduino/LVGL/hardware deps:
//
//   g++ -std=c++17 -I ../../src test_main.cpp ../../src/countdown.cpp -o t && ./t

#include "countdown.h"
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

static bool fmt_is(uint32_t s, const char* want) {
    char buf[16];
    countdown_format(s, buf, sizeof(buf));
    if (strcmp(buf, want) != 0) printf("  got \"%s\", want \"%s\"\n", buf, want);
    return strcmp(buf, want) == 0;
}

static void should_count_down_from_the_anchor() {
    Countdown c;
    countdown_anchor(&c, 10, 5000);
    CHECK(countdown_left_s(c, 5000) == 600);
    CHECK(countdown_left_s(c, 5000 + 61000) == 539);
}

static void should_keep_the_running_target_within_a_minute() {
    Countdown c;
    countdown_anchor(&c, 10, 0);
    // 60 s later the next payload says 9 min (rounded): 30 s early, kept.
    countdown_anchor(&c, 9, 30000);
    CHECK(countdown_left_s(c, 30000) == 570);
}

static void should_reanchor_beyond_a_minute() {
    Countdown c;
    countdown_anchor(&c, 10, 0);
    countdown_anchor(&c, 20, 0);
    CHECK(countdown_left_s(c, 0) == 1200);
}

static void should_hold_at_zero() {
    Countdown c;
    countdown_anchor(&c, 1, 0);
    CHECK(countdown_left_s(c, 120000) == 0);
}

static void should_survive_millis_wrap() {
    Countdown c;
    countdown_anchor(&c, 2, 0xFFFFFFFFu - 999);
    CHECK(countdown_left_s(c, 59000) == 60);
}

static void should_pick_the_later_of_two_hit_limits() {
    int mins = -1;
    CHECK(countdown_pick(false, 30, false, 900, &mins) == LIMIT_NONE);
    CHECK(countdown_pick(true, 30, false, 900, &mins) == LIMIT_SESSION && mins == 30);
    CHECK(countdown_pick(false, 30, true, 900, &mins) == LIMIT_WEEKLY && mins == 900);
    CHECK(countdown_pick(true, 30, true, 900, &mins) == LIMIT_WEEKLY && mins == 900);
    CHECK(countdown_pick(true, 30, true, 10, &mins) == LIMIT_SESSION && mins == 30);
}

static void should_format_hours_and_days() {
    CHECK(fmt_is(0, "0:00:00"));
    CHECK(fmt_is(2 * 3600 + 14 * 60 + 5, "2:14:05"));
    CHECK(fmt_is(86399, "23:59:59"));
    CHECK(fmt_is(86400, "1d 00:00"));
    CHECK(fmt_is(3 * 86400 + 4 * 3600 + 12 * 60 + 59, "3d 04:12"));
}

int main() {
    should_count_down_from_the_anchor();
    should_keep_the_running_target_within_a_minute();
    should_reanchor_beyond_a_minute();
    should_hold_at_zero();
    should_survive_millis_wrap();
    should_pick_the_later_of_two_hit_limits();
    should_format_hours_and_days();
    if (failures) {
        printf("%d countdown check(s) failed\n", failures);
        return 1;
    }
    printf("all countdown checks passed\n");
    return 0;
}
