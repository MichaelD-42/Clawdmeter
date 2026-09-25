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
#define SESSION_ROWS_MAX 4
#define AGENT_ROWS_MAX   8

struct SessionRow {
    char project[24];
    char state[8];
    char step[36];
    int  ctx;                // context window used, %; -1 = unknown
    int  n_agents;           // subagents running (may exceed those listed)
    int  first_agent;        // index into SessionInfo.agents
    int  n_listed;           // how many of them are listed there
};

struct AgentRow {
    char type[18];           // "Explore", "general-purpose", ...
    char step[36];           // what it is doing, "" before its first tool call
};

struct SessionInfo {
    char project[24];        // basename of the session's working dir
    char state[8];           // "work", "wait" (needs you), "done", "idle"
    char step[36];           // what it is doing ("Editing ui.cpp"), "" otherwise
    char model[18];          // from the status line; "" = not reported
    int  ctx;                // context window used, %; -1 = unknown
    // Every open session, the one above first (Sessions screen).
    int  n_sessions;         // open sessions (may exceed those listed)
    int  n_rows;
    SessionRow rows[SESSION_ROWS_MAX];
    int  n_agent_rows;
    AgentRow agents[AGENT_ROWS_MAX];
    // A permission prompt waiting for Allow / Deny on the board ("pr").
    uint16_t pr_id;          // 0 = none
    char pr_project[24];
    char pr_tool[24];
    char pr_preview[144];    // the command / path / URL, <= 140 chars
};
