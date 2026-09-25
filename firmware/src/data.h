#pragma once
#include <Arduino.h>

struct UsageData {
    float session_pct;       // utilization 0-100 (5h window Pro/Max; spending % Enterprise)
    int session_reset_mins;  // minutes until reset
    float weekly_pct;        // 7-day utilization (Pro/Max only; 0 for Enterprise)
    int weekly_reset_mins;   // minutes until weekly reset (Pro/Max only)
    char status[16];         // "allowed", "limited", etc.
    bool chime;              // play the session-reset chime; false unless daemon opts in
    bool enterprise;         // true = Enterprise spending-limit account
    int time_pct;            // 0-100: fraction of billing period elapsed (Enterprise)
    int period_days;         // total billing period length in days (Enterprise)
    char reset_date[12];     // formatted reset date e.g. "Jul 1" (Enterprise)
    char anim[24];           // splash animation the host wants shown ("" = host
                             // has no opinion, device picks by usage rate)
    long clock_epoch;        // local wall-clock epoch (s) from daemon; 0 = not provided
    int  clock_fmt;          // 12 or 24 (hour format from daemon); defaults to 24
    char user[24];           // account display name ("" = daemon didn't say)
    char plan[16];           // plan label, e.g. "Max 20x" ("" = unknown)
    bool ok;                 // data parse succeeded
    bool valid;              // false until first successful parse
};

// What Claude Code is doing on the host, from its hooks. Arrives as its own
// {"ev":1,...} message, separate from the usage payload.
struct SessionInfo {
    char project[24];        // basename of the session's working dir
    char state[8];           // "work", "wait" (needs you), "done", "idle"
    char tool[20];           // tool in use while working, "" otherwise
};
