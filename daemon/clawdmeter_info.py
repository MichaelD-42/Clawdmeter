#!/usr/bin/env python3
"""Host-side extras for the Clawdmeter, shared by the Linux and Windows daemons.

Five jobs, all local — no API calls:

  account  Who is logged in and on which plan, read from Claude Code's own
           account file (.claude.json -> oauthAccount).
  hook     A Claude Code hook: records what each session is doing in one small
           file per session, so concurrent hooks never write the same file.
  status   Called from the Claude Code status line: records the model and how
           full the context window is, which no hook reports.
  session  Reduces those files to what the device shows, as the {"ev":1,...}
           message the firmware understands.
  permit   A PermissionRequest hook: puts the prompt on the board and waits for
           Allow / Deny there. The terminal dialog stays up meanwhile, and
           whichever is answered first wins.

CLI (used by the bash daemon and by the hooks in ~/.claude/settings.json):
  clawdmeter_info.py hook              hook JSON on stdin
  clawdmeter_info.py status            status line JSON on stdin
  clawdmeter_info.py session           prints the session message
  clawdmeter_info.py account [DIR]     prints ,"u":..,"pl":.. (payload fragment)
  clawdmeter_info.py permit            PermissionRequest hook JSON on stdin
  clawdmeter_info.py answer ID A       the board's answer (allow|deny|pass)
"""

import json
import os
import random
import re
import signal
import sys
import time
import unicodedata
from pathlib import Path

STATE_DIR = Path.home() / ".cache" / "clawdmeter" / "sessions"
STALE_S = 2 * 3600  # a session silent this long has crashed or been abandoned
AGENT_STALE_S = 600  # a subagent that ran no tool this long has died without a stop
NAME_MAX = 20
PROJECT_MAX = 20
STEP_MAX = 32
AGENT_MAX = 16
MODEL_MAX = 16
MAX_SESSIONS = 4  # what the board has room for
MAX_AGENTS = 8  # across all sessions
MSG_MAX = 480  # the board's BLE receive buffer is 512 bytes
PREVIEW_MAX = 140  # what the board's approval overlay has room for
TOOL_MAX = 20
PERMIT_MAX_S = 590  # just inside the hook's 600 s timeout
PERMIT_POLL_S = 0.2
ANSWERS = ("allow", "deny", "pass")

if os.name == "nt":
    CONFIG_FILE = (
        Path(os.environ.get("LOCALAPPDATA", Path.home() / "AppData" / "Local"))
        / "Clawdmeter"
        / "config"
    )
else:
    CONFIG_FILE = Path.home() / ".config" / "claude-usage-monitor" / "config"

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


def _project(event: dict) -> str:
    # The workspace the session was started in. Claude Code hands every hook
    # CLAUDE_PROJECT_DIR; the event's cwd follows each `cd` into a subfolder.
    root = os.environ.get("CLAUDE_PROJECT_DIR") or event.get("cwd") or ""
    return Path(root).name[:PROJECT_MAX]


def _ascii(text) -> str:
    # The board's fonts only carry ASCII: fold accents ("Ü" -> "U"), drop the
    # rest, and squeeze whitespace (descriptions can hold newlines).
    folded = unicodedata.normalize("NFKD", str(text or ""))
    return " ".join(folded.encode("ascii", "ignore").decode().split())


def step_text(tool: str, tool_input) -> str:
    """What a tool call is doing, in a few words: the Bash description, the
    file being edited, an agent's task. The tool name only as a last resort."""
    inp = tool_input if isinstance(tool_input, dict) else {}
    base = lambda key: Path(str(inp.get(key) or "")).name  # noqa: E731
    tool = tool or ""
    if tool == "Bash":
        text = inp.get("description") or "Running a command"
    elif tool in ("Edit", "MultiEdit", "NotebookEdit"):
        text = f"Editing {base('file_path') or base('notebook_path')}"
    elif tool == "Write":
        text = f"Writing {base('file_path')}"
    elif tool == "Read":
        text = f"Reading {base('file_path')}"
    elif tool in ("Grep", "Glob"):
        text = f"Searching {inp.get('pattern') or ''}"
    elif tool in ("Agent", "Task"):
        text = inp.get("description") or "Delegating"
    elif tool == "WebFetch":
        text = "Reading " + re.sub(r"^\w+://([^/]*).*$", r"\1", str(inp.get("url") or ""))
    elif tool == "WebSearch":
        text = "Searching the web"
    elif tool == "Skill":
        text = "Using " + str(inp.get("skill") or "").split(":")[-1]
    else:
        text = tool.split("__")[-1]
    return _ascii(text)[:STEP_MAX].rstrip()


def preview_text(tool: str, tool_input) -> str:
    """What a permission prompt is about, for the board to judge by: the full
    command, the path, the URL — not the friendly step_text() summary."""
    inp = tool_input if isinstance(tool_input, dict) else {}
    tool = tool or ""
    if tool == "Bash":
        text = inp.get("command") or ""
    elif tool in ("Edit", "MultiEdit", "Write", "Read", "NotebookEdit"):
        text = inp.get("file_path") or inp.get("notebook_path") or ""
    elif tool == "WebFetch":
        text = inp.get("url") or ""
    elif tool == "WebSearch":
        text = inp.get("query") or ""
    else:
        first = next((v for v in inp.values() if isinstance(v, str) and v), "")
        text = f"{tool.split('__')[-1]} {first}"
    text = _ascii(text)
    # Say when it is cut: Allow on a command you can't see whole is a trap.
    return text if len(text) <= PREVIEW_MAX else text[: PREVIEW_MAX - 3] + "..."


def _clean(s) -> str:
    return re.sub(r"[^A-Za-z0-9_-]", "", str(s or ""))


def _write(path: Path, rec: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_name(path.name + ".tmp")
    tmp.write_text(json.dumps(rec), encoding="utf-8")
    os.replace(tmp, path)


def _read(path: Path) -> dict | None:
    try:
        rec = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return None
    return rec if isinstance(rec, dict) else None


def _remove_session(path: Path) -> None:
    # The session file plus its subagent files (<sid>.<agent>.agent) and its
    # status line record (<sid>.ctx).
    sid = path.stem
    path.unlink(missing_ok=True)
    for f in [*path.parent.glob(f"{sid}.*.agent"), path.with_suffix(".ctx")]:
        f.unlink(missing_ok=True)


def _record_agent(event: dict, kind, sid: str, now, state_dir: Path) -> None:
    # One file per subagent: parallel subagents fire hooks concurrently, and
    # a shared file would lose updates.
    aid = _clean(event.get("agent_id"))
    if not aid:
        return
    path = state_dir / f"{sid}.{aid}.agent"
    if kind == "SubagentStop":
        path.unlink(missing_ok=True)
        return
    prev = _read(path)
    if prev is None and kind != "SubagentStart":
        return  # a tool call after the stop, or from an agent we never saw start
    rec = prev or {
        "t": str(event.get("agent_type") or "").split(":")[-1][:AGENT_MAX],
        "tl": "",
        "t0": now,
    }
    if kind in ("PreToolUse", "PostToolUse"):
        rec["tl"] = step_text(event.get("tool_name"), event.get("tool_input"))
    rec["ts"] = now
    _write(path, rec)


def record_hook(event: dict, now=None, state_dir: Path = STATE_DIR) -> None:
    path = _session_file(state_dir, event.get("session_id"))
    if path is None:
        return
    now = time.time() if now is None else now
    kind = event.get("hook_event_name")
    if kind == "SessionEnd":
        _remove_session(path)
        return
    if kind in ("SubagentStart", "SubagentStop") or (
        event.get("agent_id") and kind in ("PreToolUse", "PostToolUse")
    ):
        _record_agent(event, kind, path.stem, now, state_dir)
        return
    try:
        prev = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        prev = {}
    tool = prev.get("tl", "")
    if kind == "UserPromptSubmit":
        state, tool = "work", ""
    elif kind in ("PreToolUse", "PostToolUse"):
        state, tool = "work", step_text(event.get("tool_name"), event.get("tool_input"))
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
    _write(path, {"p": _project(event), "st": state, "tl": tool, "ts": now})


def record_status(data: dict, now=None, state_dir: Path = STATE_DIR) -> None:
    path = _session_file(state_dir, data.get("session_id"))
    if path is None:
        return
    model = data.get("model")
    ctx = data.get("context_window")
    name = model.get("display_name") if isinstance(model, dict) else None
    pct = ctx.get("used_percentage") if isinstance(ctx, dict) else None
    rec = {
        "m": str(name or "")[:MODEL_MAX],
        "cx": round(pct) if isinstance(pct, (int, float)) else -1,
        "ts": time.time() if now is None else now,
    }
    _write(path.with_suffix(".ctx"), rec)


def _agents(sid: str, now, state_dir: Path) -> list:
    agents = []
    for f in state_dir.glob(f"{sid}.*.agent"):
        rec = _read(f)
        if rec is None or now - rec.get("ts", 0) > AGENT_STALE_S:
            f.unlink(missing_ok=True)
            continue
        agents.append(rec)
    agents.sort(key=lambda r: r.get("t0", 0))
    return [[a.get("t", ""), a.get("tl", "")] for a in agents]


def _fit(msg: dict) -> dict:
    # Drop agents from the bottom of the list, then whole sessions, until the
    # message fits the board's buffer. Row counts stay, so the board can still
    # say "+N more".
    def size() -> int:
        return len(json.dumps(msg, separators=(",", ":")))

    rows = msg["ss"]
    for row in reversed(rows):
        while row[4] and size() > MSG_MAX:
            row[4].pop()
    while len(rows) > 1 and size() > MSG_MAX:
        rows.pop()
    return msg


def session_summary(now=None, state_dir: Path = STATE_DIR) -> dict:
    now = time.time() if now is None else now
    sessions = []
    for f in state_dir.glob("*.json") if state_dir.is_dir() else []:
        rec = _read(f)
        if rec is None:
            continue
        if now - rec.get("ts", 0) > STALE_S:
            _remove_session(f)
            continue
        sessions.append((f.stem, rec))
    pr = pending_request(state_dir)
    if not sessions:
        return {"ev": 1, "st": "idle", **({"pr": pr} if pr else {})}
    # A session stuck on you beats a newer one that is busy on its own.
    sessions.sort(
        key=lambda s: (s[1].get("st") == "wait", s[1].get("ts", 0)), reverse=True
    )
    rows, ctxs, n_agents = [], [], 0
    for sid, rec in sessions[:MAX_SESSIONS]:
        ctx = _read(state_dir / f"{sid}.ctx") or {}
        ctxs.append(ctx)
        agents = _agents(sid, now, state_dir)
        shown = agents[: max(0, MAX_AGENTS - n_agents)]
        n_agents += len(shown)
        rows.append(
            [
                rec.get("p", ""),
                rec.get("st", "idle"),
                rec.get("tl", ""),
                ctx.get("cx", -1),
                shown,
                len(agents),
            ]
        )
    best, ctx = sessions[0][1], ctxs[0]
    msg = {
        "ev": 1,
        "p": best.get("p", ""),
        "st": best.get("st", "idle"),
        "tl": best.get("tl", ""),
    }
    if ctx:
        msg["m"] = ctx.get("m", "")
        msg["cx"] = ctx.get("cx", -1)
    msg["sn"] = len(sessions)
    msg["ss"] = rows
    if pr:
        msg["pr"] = pr
    return _fit(msg)


# ---- Permission requests ---------------------------------------------------
# The hook writes <id>.req next to the session files (so the bash daemon's
# dir-mtime check sees it) and holds an OS lock on <id>.lock for as long as it
# waits. Answering in the terminal kills the hook without any cleanup, but the
# lock dies with the process — that is how session_summary() tells a live
# request from a leftover one. The daemon writes the board's answer to <id>.ans.


def _lock(path: Path):
    """Open and exclusively lock `path`; the open file, or None if it is held."""
    f = open(path, "a+b")
    try:
        if os.name == "nt":
            import msvcrt

            f.seek(0)
            msvcrt.locking(f.fileno(), msvcrt.LK_NBLCK, 1)
        else:
            import fcntl

            fcntl.flock(f, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except OSError:
        f.close()
        return None
    return f


def _hook_gone(lock: Path) -> bool:
    # Getting the lock means the hook that held it is no longer running.
    probe = _lock(lock) if lock.exists() else None
    if probe is not None:
        probe.close()
    return probe is not None or not lock.exists()


def _drop_request(state_dir: Path, rid: str) -> None:
    for ext in ("req", "ans", "lock"):
        (state_dir / f"{rid}.{ext}").unlink(missing_ok=True)


def _decision(answer: str) -> dict:
    decision = {"behavior": answer}
    if answer == "deny":
        decision["message"] = "Denied on Clawdmeter"
    out = {"hookEventName": "PermissionRequest", "decision": decision}
    return {"hookSpecificOutput": out}


def request_permission(
    event: dict,
    state_dir: Path = STATE_DIR,
    wait=time.sleep,
    approve: bool = True,
    max_s: float = PERMIT_MAX_S,
    poll_s: float = PERMIT_POLL_S,
) -> dict | None:
    """Put the prompt on the board and wait for its answer. Returns the hook
    output for allow/deny, None to leave it to the terminal."""
    if not approve:
        return None
    state_dir.mkdir(parents=True, exist_ok=True)
    while True:
        rid = f"{random.randint(1, 0xFFFF):04x}"
        if not (state_dir / f"{rid}.lock").exists():
            break
    held = _lock(state_dir / f"{rid}.lock")
    if held is None:
        return None
    try:
        tool = event.get("tool_name") or ""
        _write(
            state_dir / f"{rid}.req",
            {
                "sid": _clean(event.get("session_id")),
                "p": _project(event),
                "t": _ascii(tool.split("__")[-1])[:TOOL_MAX],
                "v": preview_text(tool, event.get("tool_input")),
                "ts": time.time(),
            },
        )
        ans_path = state_dir / f"{rid}.ans"
        polls = round(max_s / poll_s)
        for i in range(polls + 1):
            try:
                answer = ans_path.read_text(encoding="utf-8").strip()
            except OSError:
                answer = ""
            if answer in ("allow", "deny"):
                return _decision(answer)
            if answer == "pass":
                return None
            if i < polls:
                wait(poll_s)
        return None
    finally:
        held.close()
        _drop_request(state_dir, rid)


def write_answer(rid: str, answer: str, state_dir: Path = STATE_DIR) -> bool:
    """The board's answer, for the waiting hook. Ignored unless the request
    is still there."""
    if not re.fullmatch(r"[0-9a-f]{4}", rid or "") or answer not in ANSWERS:
        return False
    if not (state_dir / f"{rid}.req").exists():
        return False
    (state_dir / f"{rid}.ans").write_text(answer, encoding="utf-8")
    return True


def pending_request(state_dir: Path = STATE_DIR) -> list | None:
    """The oldest request whose hook is still waiting, as [id, project, tool,
    preview]; leftovers of killed hooks are removed on the way."""
    live = []
    for f in state_dir.glob("*.req") if state_dir.is_dir() else []:
        rid = f.stem
        if _hook_gone(state_dir / f"{rid}.lock"):
            _drop_request(state_dir, rid)
            continue
        rec = _read(f)
        if rec is not None:
            live.append((rec.get("ts", 0), rid, rec))
    if not live:
        return None
    _, rid, rec = min(live)
    return [int(rid, 16), rec.get("p", ""), rec.get("t", ""), rec.get("v", "")]


def approve_enabled(config_file: Path = CONFIG_FILE) -> bool:
    # `approve = off` in the daemon config turns the board's Allow / Deny off.
    try:
        lines = config_file.read_text(encoding="utf-8").splitlines()
    except OSError:
        return True
    value = "on"
    for line in lines:
        m = re.match(r"\s*approve\s*=\s*(\S*)", line)
        if m:
            value = m.group(1).lower()
    return value != "off"


def main(argv) -> int:
    cmd = argv[1] if len(argv) > 1 else ""
    if cmd == "hook":
        # Never get in Claude's way: whatever happens, exit 0 with no output.
        try:
            record_hook(json.load(sys.stdin))
        except Exception:
            pass
        return 0
    if cmd == "status":
        # Runs on every status line refresh: same rules as the hook.
        try:
            record_status(json.load(sys.stdin))
        except Exception:
            pass
        return 0
    if cmd == "permit":
        # Answering in the terminal ends us with SIGTERM: exit through the
        # finally that removes the request. Never fail in Claude's way.
        try:
            signal.signal(signal.SIGTERM, lambda *_: sys.exit(0))
            event = json.load(sys.stdin)
            out = request_permission(event, approve=approve_enabled())
            if out:
                print(json.dumps(out))
        except Exception:
            pass
        return 0
    if cmd == "answer":
        if len(argv) != 4:
            print(__doc__, file=sys.stderr)
            return 2
        return 0 if write_answer(argv[2], argv[3], state_dir=STATE_DIR) else 1
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
