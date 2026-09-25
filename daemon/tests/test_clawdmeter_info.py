#!/usr/bin/env python3
"""Tests for clawdmeter_info — account label, hook recorder, session summary.

Run: python -m pytest daemon/tests/test_clawdmeter_info.py -q
"""

import json

import pytest

from daemon import clawdmeter_info as ci


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
        "tl": "Bash",
    }


def test_mcp_tool_name_is_shortened(tmp_path):
    _hook(tmp_path, 100, "PreToolUse", tool_name="mcp__claude-in-chrome__navigate")
    assert ci.session_summary(now=101, state_dir=tmp_path)["tl"] == "navigate"


def test_permission_prompt_waits_and_keeps_tool(tmp_path):
    _hook(tmp_path, 100, "PreToolUse", tool_name="Bash")
    _hook(tmp_path, 101, "Notification", notification_type="permission_prompt")
    s = ci.session_summary(now=102, state_dir=tmp_path)
    assert s["st"] == "wait" and s["tl"] == "Bash"


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
