#!/usr/bin/env python3
"""Tests for clawdmeter_info — account label, hook recorder, session summary.

Run: python -m pytest daemon/tests/test_clawdmeter_info.py -q
"""

import json

import pytest

from daemon import clawdmeter_info as ci


@pytest.fixture(autouse=True)
def _no_project_dir(monkeypatch):
    # Set when pytest runs inside a Claude Code session; the tests name
    # projects through the event's cwd unless they set it themselves.
    monkeypatch.delenv("CLAUDE_PROJECT_DIR", raising=False)


# ---------------------------------------------------------------------------
# plan_label / account_fields
# ---------------------------------------------------------------------------


@pytest.mark.parametrize(
    "org, tier, want",
    [
        ("claude_pro", "default_claude_ai", "Pro"),
        ("claude_max", "default_claude_max_20x", "Max 20x"),
        ("claude_max", "default_claude_max_5x", "Max 5x"),
        ("claude_max", None, "Max"),
        ("claude_team", None, "Team"),
        ("claude_enterprise", None, "Enterprise"),
        (None, None, ""),
        ("something_new", None, ""),
    ],
)
def test_plan_label(org, tier, want):
    assert ci.plan_label(org, tier) == want


def _write_account(path, **acct):
    path.write_text(json.dumps({"oauthAccount": acct, "other": 1}))


def test_account_fields_from_config_dir(tmp_path):
    _write_account(
        tmp_path / ".claude.json",
        displayName="Michael",
        organizationType="claude_max",
        organizationRateLimitTier="default_claude_max_5x",
    )
    assert ci.account_fields(tmp_path) == {"u": "Michael", "pl": "Max 5x"}


def test_account_fields_default_dir_reads_home_file(tmp_path, monkeypatch):
    monkeypatch.setattr(ci.Path, "home", lambda: tmp_path)
    _write_account(
        tmp_path / ".claude.json", displayName="M", organizationType="claude_pro"
    )
    # ~/.claude is the "no CLAUDE_CONFIG_DIR" case: its account file is ~/.claude.json
    assert ci.account_fields(tmp_path / ".claude") == {"u": "M", "pl": "Pro"}
    assert ci.account_fields(None) == {"u": "M", "pl": "Pro"}


def test_account_fields_truncates_name(tmp_path):
    _write_account(
        tmp_path / ".claude.json", displayName="A" * 40, organizationType="claude_pro"
    )
    assert ci.account_fields(tmp_path)["u"] == "A" * 20


def test_account_fields_missing_or_broken_file(tmp_path):
    assert ci.account_fields(tmp_path) == {}
    (tmp_path / ".claude.json").write_text("{not json")
    assert ci.account_fields(tmp_path) == {}


# ---------------------------------------------------------------------------
# record_hook / session_summary
# ---------------------------------------------------------------------------


def _hook(d, now, event, sid="s1", cwd="/home/x/Clawdmeter", **extra):
    ci.record_hook(
        {"hook_event_name": event, "session_id": sid, "cwd": cwd, **extra},
        now=now,
        state_dir=d,
    )


def test_no_sessions_is_idle(tmp_path):
    assert ci.session_summary(now=100, state_dir=tmp_path / "none") == {
        "ev": 1,
        "st": "idle",
    }


def test_tool_use_is_working_with_tool(tmp_path):
    _hook(tmp_path, 100, "PreToolUse", tool_name="Bash")
    assert ci.session_summary(now=101, state_dir=tmp_path) == {
        "ev": 1,
        "p": "Clawdmeter",
        "st": "work",
        "tl": "Running a command",
        "sn": 1,
        "ss": [["Clawdmeter", "work", "Running a command", -1, [], 0]],
    }


@pytest.mark.parametrize(
    "tool, tool_input, want",
    [
        ("Bash", {"command": "pio run", "description": "Build all seven board envs"},
         "Build all seven board envs"),
        ("Bash", {"command": "ls"}, "Running a command"),
        ("Edit", {"file_path": "/x/firmware/src/ui.cpp"}, "Editing ui.cpp"),
        ("Write", {"file_path": "/x/README.md"}, "Writing README.md"),
        ("Read", {"file_path": "/x/data.h"}, "Reading data.h"),
        ("Grep", {"pattern": "pct_color"}, "Searching pct_color"),
        ("Glob", {"pattern": "**/*.cpp"}, "Searching **/*.cpp"),
        ("Agent", {"description": "Find tap handling", "prompt": "..."}, "Find tap handling"),
        ("Task", {"description": "Review diff"}, "Review diff"),
        ("WebFetch", {"url": "https://code.claude.com/docs/en/hooks"}, "Reading code.claude.com"),
        ("WebSearch", {"query": "lvgl label"}, "Searching the web"),
        ("Skill", {"skill": "superpowers:brainstorming"}, "Using brainstorming"),
        ("mcp__home-assistant__ha_get_state", {}, "ha_get_state"),
        ("SomethingNew", {}, "SomethingNew"),
        # Long and non-ASCII text: cut, and only what the board's fonts can draw.
        ("Bash", {"description": "Überprüfe  alle\nsieben Boards und dann noch viel mehr Text"},
         "Uberprufe alle sieben Boards und"),
    ],
)
def test_step_text(tool, tool_input, want):
    assert ci.step_text(tool, tool_input) == want


def test_mcp_tool_name_is_shortened(tmp_path):
    _hook(tmp_path, 100, "PreToolUse", tool_name="mcp__claude-in-chrome__navigate")
    assert ci.session_summary(now=101, state_dir=tmp_path)["tl"] == "navigate"


def test_permission_prompt_waits_and_keeps_tool(tmp_path):
    _hook(tmp_path, 100, "PreToolUse", tool_name="Bash")
    _hook(tmp_path, 101, "Notification", notification_type="permission_prompt")
    s = ci.session_summary(now=102, state_dir=tmp_path)
    assert s["st"] == "wait" and s["tl"] == "Running a command"


def test_post_tool_use_clears_wait(tmp_path):
    _hook(tmp_path, 100, "Notification", notification_type="permission_prompt")
    _hook(tmp_path, 101, "PostToolUse", tool_name="Bash")
    assert ci.session_summary(now=102, state_dir=tmp_path)["st"] == "work"


def test_stop_is_done_and_idle_prompt_does_not_alert(tmp_path):
    _hook(tmp_path, 100, "Stop")
    _hook(tmp_path, 160, "Notification", notification_type="idle_prompt")
    assert ci.session_summary(now=161, state_dir=tmp_path)["st"] == "done"


def test_unrelated_notification_is_ignored(tmp_path):
    _hook(tmp_path, 100, "PreToolUse", tool_name="Edit")
    _hook(tmp_path, 101, "Notification", notification_type="auth_success")
    assert ci.session_summary(now=102, state_dir=tmp_path)["st"] == "work"


def test_waiting_session_wins_over_newer_busy_one(tmp_path):
    _hook(
        tmp_path,
        100,
        "Notification",
        sid="a",
        cwd="/p/alpha",
        notification_type="permission_prompt",
    )
    _hook(tmp_path, 200, "PreToolUse", sid="b", cwd="/p/beta", tool_name="Edit")
    s = ci.session_summary(now=201, state_dir=tmp_path)
    assert s["st"] == "wait" and s["p"] == "alpha"


def test_newest_session_shown_when_nobody_waits(tmp_path):
    _hook(tmp_path, 100, "Stop", sid="a", cwd="/p/alpha")
    _hook(tmp_path, 200, "PreToolUse", sid="b", cwd="/p/beta", tool_name="Edit")
    assert ci.session_summary(now=201, state_dir=tmp_path)["p"] == "beta"


def test_session_end_removes_session(tmp_path):
    _hook(tmp_path, 100, "PreToolUse", tool_name="Bash")
    _hook(tmp_path, 101, "SessionEnd", reason="prompt_input_exit")
    assert ci.session_summary(now=102, state_dir=tmp_path) == {"ev": 1, "st": "idle"}


def test_stale_session_is_dropped(tmp_path):
    _hook(tmp_path, 100, "PreToolUse", tool_name="Bash")  # e.g. a crashed session
    assert ci.session_summary(now=100 + ci.STALE_S + 1, state_dir=tmp_path) == {
        "ev": 1,
        "st": "idle",
    }


def test_garbage_hook_input_is_harmless(tmp_path):
    ci.record_hook({}, now=100, state_dir=tmp_path)
    ci.record_hook(
        {"hook_event_name": "PreToolUse", "session_id": "../../etc"},
        now=100,
        state_dir=tmp_path,
    )
    assert not (tmp_path.parent / "etc.json").exists()


# ---------------------------------------------------------------------------
# subagents
# ---------------------------------------------------------------------------


def _agent(d, now, event, aid, atype="Explore", sid="s1", **extra):
    _hook(d, now, event, sid=sid, agent_id=aid, agent_type=atype, **extra)


def test_subagent_start_and_stop(tmp_path):
    _hook(tmp_path, 100, "PreToolUse", tool_name="Agent")
    _agent(tmp_path, 101, "SubagentStart", "a1")
    s = ci.session_summary(now=102, state_dir=tmp_path)
    assert s["ss"][0][4] == [["Explore", ""]]
    _agent(tmp_path, 103, "SubagentStop", "a1")
    assert ci.session_summary(now=104, state_dir=tmp_path)["ss"][0][4] == []


def test_subagent_tool_does_not_touch_main_session(tmp_path):
    _hook(tmp_path, 100, "PreToolUse", tool_name="Agent")
    _agent(tmp_path, 101, "SubagentStart", "a1")
    _agent(tmp_path, 102, "PreToolUse", "a1", tool_name="Grep")
    s = ci.session_summary(now=103, state_dir=tmp_path)
    assert s["tl"] == "Delegating"
    assert s["ss"][0][4] == [["Explore", "Searching"]]


def test_parallel_subagents_listed_oldest_first(tmp_path):
    _hook(tmp_path, 100, "PreToolUse", tool_name="Agent")
    _agent(tmp_path, 101, "SubagentStart", "a1", atype="Explore")
    _agent(tmp_path, 102, "SubagentStart", "a2", atype="plugin:x:reviewer")
    _agent(tmp_path, 103, "PostToolUse", "a1", tool_name="Read")
    s = ci.session_summary(now=104, state_dir=tmp_path)
    assert s["ss"][0][4] == [["Explore", "Reading"], ["reviewer", ""]]


def test_silent_subagent_is_dropped(tmp_path):
    _hook(tmp_path, 100, "PreToolUse", tool_name="Agent")
    _agent(tmp_path, 100, "SubagentStart", "a1")
    now = 100 + ci.AGENT_STALE_S + 1
    _hook(tmp_path, now, "PreToolUse", tool_name="Bash")  # session itself alive
    assert ci.session_summary(now=now, state_dir=tmp_path)["ss"][0][4] == []
    assert not list(tmp_path.glob("*.agent"))


def test_session_end_removes_agents_and_status(tmp_path):
    _hook(tmp_path, 100, "PreToolUse", tool_name="Agent")
    _agent(tmp_path, 101, "SubagentStart", "a1")
    ci.record_status(
        {"session_id": "s1", "model": {"display_name": "Opus 5.5"}},
        now=101,
        state_dir=tmp_path,
    )
    _hook(tmp_path, 102, "SessionEnd")
    assert list(tmp_path.iterdir()) == []


# ---------------------------------------------------------------------------
# status line: model + context
# ---------------------------------------------------------------------------


def _status(d, now, sid="s1", model="Opus 5.5", pct=42.7):
    ci.record_status(
        {
            "session_id": sid,
            "model": {"id": "claude-opus-5-5", "display_name": model},
            "context_window": {"used_percentage": pct},
        },
        now=now,
        state_dir=d,
    )


def test_status_line_records_model_and_context(tmp_path):
    _hook(tmp_path, 100, "PreToolUse", tool_name="Bash")
    _status(tmp_path, 101)
    s = ci.session_summary(now=102, state_dir=tmp_path)
    assert s["m"] == "Opus 5.5" and s["cx"] == 43
    assert s["ss"][0][3] == 43


def test_status_line_without_context_yet(tmp_path):
    _hook(tmp_path, 100, "PreToolUse", tool_name="Bash")
    _status(tmp_path, 101, pct=None)
    s = ci.session_summary(now=102, state_dir=tmp_path)
    assert s["m"] == "Opus 5.5" and s["cx"] == -1


def test_status_alone_does_not_make_a_session(tmp_path):
    _status(tmp_path, 100)
    assert ci.session_summary(now=101, state_dir=tmp_path) == {"ev": 1, "st": "idle"}


def test_garbage_status_input_is_harmless(tmp_path):
    ci.record_status({}, now=100, state_dir=tmp_path)
    ci.record_status({"session_id": "s1", "model": "x"}, now=100, state_dir=tmp_path)
    ci.record_status(
        {"session_id": "s1", "context_window": {"used_percentage": "lots"}},
        now=100,
        state_dir=tmp_path,
    )
    assert ci.main(["x", "status"]) == 0


# ---------------------------------------------------------------------------
# session list
# ---------------------------------------------------------------------------


def test_session_list_waiting_first_then_newest(tmp_path):
    _hook(tmp_path, 100, "Stop", sid="a", cwd="/p/alpha")
    _hook(tmp_path, 110, "Notification", sid="b", cwd="/p/beta",
          notification_type="permission_prompt")
    _hook(tmp_path, 120, "PreToolUse", sid="c", cwd="/p/gamma", tool_name="Edit")
    s = ci.session_summary(now=121, state_dir=tmp_path)
    assert [r[0] for r in s["ss"]] == ["beta", "gamma", "alpha"]
    assert s["p"] == "beta"


def test_session_list_is_capped(tmp_path):
    for i in range(ci.MAX_SESSIONS + 2):
        _hook(tmp_path, 100 + i, "PreToolUse", sid=f"s{i}", cwd=f"/p/p{i}", tool_name="Bash")
    assert len(ci.session_summary(now=200, state_dir=tmp_path)["ss"]) == ci.MAX_SESSIONS


def test_message_fits_the_board_buffer(tmp_path):
    for i in range(ci.MAX_SESSIONS):
        sid = f"s{i}"
        _hook(tmp_path, 100, "PreToolUse", sid=sid, cwd="/p/" + "x" * 40,
              tool_name="mcp__server__" + "t" * 40)
        _status(tmp_path, 100, sid=sid, model="M" * 40)
        for a in range(ci.MAX_AGENTS):
            _agent(tmp_path, 100 + a, "SubagentStart", f"a{a}", sid=sid,
                   atype="general-purpose-long-name")
            _agent(tmp_path, 100 + a, "PreToolUse", f"a{a}", sid=sid,
                   tool_name="mcp__server__" + "t" * 40)
    s = ci.session_summary(now=120, state_dir=tmp_path)
    assert len(json.dumps(s, separators=(",", ":"))) <= ci.MSG_MAX
    assert s["ss"]  # trimmed, not emptied


# ---------------------------------------------------------------------------
# permission requests (the board's Allow / Deny)
# ---------------------------------------------------------------------------


@pytest.mark.parametrize(
    "tool, inp, want",
    [
        ("Bash", {"command": "rm -rf build && make", "description": "Rebuild"},
         "rm -rf build && make"),
        ("Edit", {"file_path": "/home/x/Clawdmeter/ui.cpp"}, "/home/x/Clawdmeter/ui.cpp"),
        ("Write", {"file_path": "/tmp/a.txt", "content": "x"}, "/tmp/a.txt"),
        ("WebFetch", {"url": "https://example.com/a", "prompt": "t"}, "https://example.com/a"),
        ("WebSearch", {"query": "esp32 c6 psram"}, "esp32 c6 psram"),
        ("mcp__srv__do_thing", {"n": 3, "target": "prod"}, "do_thing prod"),
        ("Bash", {"command": "echo 'Grüße'\nls"}, "echo 'Grue' ls"),
        ("Odd", None, "Odd"),
    ],
)
def test_preview_text(tool, inp, want):
    assert ci.preview_text(tool, inp) == want


def test_preview_text_is_capped_and_says_so():
    # An approval screen must never show a cut command as if it were whole.
    text = ci.preview_text("Bash", {"command": "x" * 500})
    assert len(text) == ci.PREVIEW_MAX
    assert text.endswith("...")
    assert ci.preview_text("Bash", {"command": "y" * ci.PREVIEW_MAX}) == "y" * ci.PREVIEW_MAX


_PERM_EVENT = {
    "hook_event_name": "PermissionRequest",
    "session_id": "s1",
    "cwd": "/home/x/Clawdmeter",
    "tool_name": "Bash",
    "tool_input": {"command": "make flash"},
}


def _req_ids(d):
    return sorted(p.stem for p in d.glob("*.req"))


def _answer_on_first_wait(d, answer):
    # Stands in for the daemon: answers the pending request while the hook waits.
    def wait(_s):
        for rid in _req_ids(d):
            ci.write_answer(rid, answer, state_dir=d)
    return wait


@pytest.mark.parametrize("answer", ["allow", "deny"])
def test_permit_returns_the_boards_answer_and_cleans_up(tmp_path, answer):
    out = ci.request_permission(
        _PERM_EVENT, state_dir=tmp_path, wait=_answer_on_first_wait(tmp_path, answer)
    )
    decision = out["hookSpecificOutput"]["decision"]
    assert out["hookSpecificOutput"]["hookEventName"] == "PermissionRequest"
    assert decision["behavior"] == answer
    if answer == "deny":
        assert decision["message"] == "Denied on Clawdmeter"
    assert list(tmp_path.iterdir()) == []


def test_permit_pass_leaves_it_to_the_terminal(tmp_path):
    out = ci.request_permission(
        _PERM_EVENT, state_dir=tmp_path, wait=_answer_on_first_wait(tmp_path, "pass")
    )
    assert out is None
    assert list(tmp_path.iterdir()) == []


def test_permit_gives_up_after_max_wait(tmp_path):
    calls = []
    out = ci.request_permission(
        _PERM_EVENT, state_dir=tmp_path, wait=calls.append, max_s=1.0, poll_s=0.25
    )
    assert out is None
    assert len(calls) == 4
    assert list(tmp_path.iterdir()) == []


def test_permit_is_off_when_disabled(tmp_path):
    def wait(_s):
        raise AssertionError("must not wait")
    assert ci.request_permission(_PERM_EVENT, state_dir=tmp_path, wait=wait,
                                 approve=False) is None
    assert list(tmp_path.iterdir()) == []


def test_pending_request_is_in_the_session_message(tmp_path):
    _hook(tmp_path, 100, "PreToolUse", tool_name="Bash")
    seen = {}

    def wait(_s):
        seen["msg"] = ci.session_summary(now=101, state_dir=tmp_path)
        for rid in _req_ids(tmp_path):
            ci.write_answer(rid, "allow", state_dir=tmp_path)

    ci.request_permission(_PERM_EVENT, state_dir=tmp_path, wait=wait)
    rid, project, tool, preview = seen["msg"]["pr"]
    assert isinstance(rid, int) and 0 < rid <= 0xFFFF
    assert (project, tool, preview) == ("Clawdmeter", "Bash", "make flash")
    assert "pr" not in ci.session_summary(now=102, state_dir=tmp_path)


def test_request_of_a_killed_hook_is_retired(tmp_path):
    # Answering in the terminal kills the hook: its lock goes with it, and the
    # leftover request must not keep the overlay on the board.
    _hook(tmp_path, 100, "PreToolUse", tool_name="Bash")
    ci._write(tmp_path / "00ab.req", {"sid": "s1", "p": "x", "t": "Bash", "v": "ls", "ts": 100})
    (tmp_path / "00ab.lock").touch()
    assert "pr" not in ci.session_summary(now=101, state_dir=tmp_path)
    assert _req_ids(tmp_path) == []
    assert not (tmp_path / "00ab.lock").exists()


def test_answer_for_an_unknown_request_is_ignored(tmp_path):
    assert ci.write_answer("beef", "allow", state_dir=tmp_path) is False
    assert list(tmp_path.iterdir()) == []


def test_answer_rejects_bad_input(tmp_path):
    ci._write(tmp_path / "00ab.req", {"ts": 1})
    assert ci.write_answer("00ab", "maybe", state_dir=tmp_path) is False
    assert ci.write_answer("../00ab", "allow", state_dir=tmp_path) is False
    assert not (tmp_path / "00ab.ans").exists()


def test_session_message_with_request_still_fits(tmp_path):
    for i in range(ci.MAX_SESSIONS):
        sid = f"s{i}"
        _hook(tmp_path, 100, "PreToolUse", sid=sid, cwd="/p/" + "x" * 40,
              tool_name="mcp__server__" + "t" * 40)
        for a in range(ci.MAX_AGENTS):
            _agent(tmp_path, 100 + a, "SubagentStart", f"a{a}", sid=sid,
                   atype="general-purpose-long-name")
    ev = dict(_PERM_EVENT, tool_input={"command": "y" * 500})

    def wait(_s):
        seen = ci.session_summary(now=120, state_dir=tmp_path)
        assert len(json.dumps(seen, separators=(",", ":"))) <= ci.MSG_MAX
        assert seen["pr"][3] == "y" * (ci.PREVIEW_MAX - 3) + "..."
        for rid in _req_ids(tmp_path):
            ci.write_answer(rid, "deny", state_dir=tmp_path)

    ci.request_permission(ev, state_dir=tmp_path, wait=wait)


@pytest.mark.parametrize(
    "text, want",
    [(None, True), ("", True), ("approve = off\n", False), ("approve=on\n", True),
     ("chime = on\napprove = OFF  \n", False), ("approve = off\napprove = on\n", True)],
)
def test_approve_setting(tmp_path, text, want):
    cfg = tmp_path / "config"
    if text is not None:
        cfg.write_text(text)
    assert ci.approve_enabled(cfg) is want


def test_answer_cli(tmp_path, monkeypatch):
    monkeypatch.setattr(ci, "STATE_DIR", tmp_path)
    ci._write(tmp_path / "00ab.req", {"ts": 1})
    assert ci.main(["x", "answer", "00ab", "allow"]) == 0
    assert (tmp_path / "00ab.ans").read_text().strip() == "allow"
    assert ci.main(["x", "answer", "00ab"]) == 2


def test_project_is_the_workspace_root_not_the_cwd(tmp_path, monkeypatch):
    # A `cd` into a subfolder moves the hook's cwd; the session keeps its name.
    monkeypatch.setenv("CLAUDE_PROJECT_DIR", "/home/x/Clawdmeter")
    _hook(tmp_path, 100, "PreToolUse", cwd="/home/x/Clawdmeter/firmware/src", tool_name="Bash")
    assert ci.session_summary(now=101, state_dir=tmp_path)["p"] == "Clawdmeter"


def test_project_falls_back_to_cwd(tmp_path, monkeypatch):
    monkeypatch.delenv("CLAUDE_PROJECT_DIR", raising=False)
    _hook(tmp_path, 100, "PreToolUse", cwd="/home/x/other", tool_name="Bash")
    assert ci.session_summary(now=101, state_dir=tmp_path)["p"] == "other"


def test_permission_request_names_the_workspace_root(tmp_path, monkeypatch):
    monkeypatch.setenv("CLAUDE_PROJECT_DIR", "/home/x/Clawdmeter")
    ev = dict(_PERM_EVENT, cwd="/home/x/Clawdmeter/daemon")
    seen = {}

    def wait(_s):
        seen["pr"] = ci.pending_request(tmp_path)
        for rid in _req_ids(tmp_path):
            ci.write_answer(rid, "pass", state_dir=tmp_path)

    ci.request_permission(ev, state_dir=tmp_path, wait=wait)
    assert seen["pr"][1] == "Clawdmeter"
