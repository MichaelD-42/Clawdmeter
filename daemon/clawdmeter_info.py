#!/usr/bin/env python3
"""Host-side extras for the Clawdmeter, shared by the Linux and Windows daemons.

Three jobs, all local — no API calls:

  account  Who is logged in and on which plan, read from Claude Code's own
           account file (.claude.json -> oauthAccount).
  hook     A Claude Code hook: records what each session is doing in one small
           file per session, so concurrent hooks never write the same file.
  session  Reduces those files to the one line the device shows, as the
           {"ev":1,...} message the firmware understands.

CLI (used by the bash daemon and by the hooks in ~/.claude/settings.json):
  clawdmeter_info.py hook              hook JSON on stdin
  clawdmeter_info.py session           prints the session message
  clawdmeter_info.py account [DIR]     prints ,"u":..,"pl":.. (payload fragment)
"""

import json
import os
import re
import sys
import time
from pathlib import Path

STATE_DIR = Path.home() / ".cache" / "clawdmeter" / "sessions"
STALE_S = 2 * 3600  # a session silent this long has crashed or been abandoned
NAME_MAX = 20
PROJECT_MAX = 20
TOOL_MAX = 16

# Notification types where Claude is stuck until you act. idle_prompt is not
# one of them: it fires a minute after a finished turn, which "done" already says.
WAIT_TYPES = {
    "permission_prompt",
    "elicitation_dialog",
    "elicitation_url_dialog",
    "agent_needs_input",
}

PLANS = {
    "claude_pro": "Pro",
    "claude_max": "Max",
    "claude_team": "Team",
    "claude_enterprise": "Enterprise",
}


def plan_label(org_type, tier) -> str:
    label = PLANS.get(org_type or "", "")
    if label == "Max":
        m = re.search(r"(\d+x)", tier or "")
        if m:
            label = f"Max {m.group(1)}"
    return label


def _account_file(config_dir) -> Path:
    # Without CLAUDE_CONFIG_DIR, Claude Code keeps the account in ~/.claude.json,
    # next to (not inside) ~/.claude. With it, the file lives in that dir.
    home = Path.home()
    if config_dir is None or Path(config_dir) == home / ".claude":
        return home / ".claude.json"
    return Path(config_dir) / ".claude.json"


def account_fields(config_dir=None) -> dict:
    try:
        acct = json.loads(_account_file(config_dir).read_text(encoding="utf-8"))[
            "oauthAccount"
        ]
    except (OSError, ValueError, KeyError, TypeError):
        return {}
    out = {}
    name = (acct.get("displayName") or "").strip()[:NAME_MAX]
    if name:
        out["u"] = name
    plan = plan_label(
        acct.get("organizationType"),
        acct.get("organizationRateLimitTier") or acct.get("userRateLimitTier"),
    )
    if plan:
        out["pl"] = plan
    return out


def _session_file(state_dir: Path, session_id) -> Path | None:
    sid = re.sub(r"[^A-Za-z0-9_-]", "", str(session_id or ""))
    return state_dir / f"{sid}.json" if sid else None


def _short_tool(name: str) -> str:
    return (name or "").split("__")[-1][:TOOL_MAX]


def record_hook(event: dict, now=None, state_dir: Path = STATE_DIR) -> None:
    path = _session_file(state_dir, event.get("session_id"))
    if path is None:
        return
    kind = event.get("hook_event_name")
    if kind == "SessionEnd":
        path.unlink(missing_ok=True)
        return
    try:
        prev = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        prev = {}
    tool = prev.get("tl", "")
    if kind == "UserPromptSubmit":
        state, tool = "work", ""
    elif kind in ("PreToolUse", "PostToolUse"):
        state, tool = "work", _short_tool(event.get("tool_name", ""))
    elif kind == "Notification":
        ntype = event.get("notification_type")
        if ntype in WAIT_TYPES:
            state = "wait"
        elif ntype == "idle_prompt":
            state = "done"
        else:
            return
    elif kind == "Stop":
        state, tool = "done", ""
    else:
        return
    project = Path(event.get("cwd") or "").name[:PROJECT_MAX]
    rec = {
        "p": project,
        "st": state,
        "tl": tool,
        "ts": time.time() if now is None else now,
    }
    state_dir.mkdir(parents=True, exist_ok=True)
    tmp = path.with_suffix(".tmp")
    tmp.write_text(json.dumps(rec), encoding="utf-8")
    os.replace(tmp, path)


def session_summary(now=None, state_dir: Path = STATE_DIR) -> dict:
    now = time.time() if now is None else now
    recs = []
    for f in state_dir.glob("*.json") if state_dir.is_dir() else []:
        try:
            rec = json.loads(f.read_text(encoding="utf-8"))
        except (OSError, ValueError):
            continue
        if now - rec.get("ts", 0) > STALE_S:
            f.unlink(missing_ok=True)
            continue
        recs.append(rec)
    if not recs:
        return {"ev": 1, "st": "idle"}
    # A session stuck on you beats a newer one that is busy on its own.
    waiting = [r for r in recs if r.get("st") == "wait"]
    best = max(waiting or recs, key=lambda r: r.get("ts", 0))
    return {
        "ev": 1,
        "p": best.get("p", ""),
        "st": best.get("st", "idle"),
        "tl": best.get("tl", ""),
    }


def main(argv) -> int:
    cmd = argv[1] if len(argv) > 1 else ""
    if cmd == "hook":
        # Never get in Claude's way: whatever happens, exit 0 with no output.
        try:
            record_hook(json.load(sys.stdin))
        except Exception:
            pass
        return 0
    if cmd == "session":
        print(json.dumps(session_summary(), separators=(",", ":")))
        return 0
    if cmd == "account":
        fields = account_fields(argv[2] if len(argv) > 2 else None)
        frag = json.dumps(fields, separators=(",", ":"))[1:-1]
        print("," + frag if frag else "")
        return 0
    print(__doc__, file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
