#!/usr/bin/env python3
"""The board's REQ notifies in the Windows daemon: 01 = refresh, 02/03 lo hi =
Allow / Deny on permission request <hi><lo>.

Run: python -m pytest daemon/tests/test_windows_answer.py -q
"""

from unittest.mock import MagicMock

import pytest

from daemon import clawdmeter_info as ci
from daemon.claude_usage_daemon_windows import Session


@pytest.fixture
def session(tmp_path, monkeypatch):
    monkeypatch.setattr(ci, "STATE_DIR", tmp_path)
    return Session(MagicMock())


def test_refresh_notify_sets_the_event(session):
    session._on_refresh(None, bytearray(b"\x01"))
    assert session.refresh_requested.is_set()


@pytest.mark.parametrize("op, want", [(0x02, "allow"), (0x03, "deny")])
def test_answer_notify_writes_the_answer(session, tmp_path, op, want):
    ci._write(tmp_path / "c001.req", {"ts": 1})
    session._on_refresh(None, bytearray([op, 0x01, 0xC0]))
    assert (tmp_path / "c001.ans").read_text() == want
    assert not session.refresh_requested.is_set()


def test_malformed_notify_is_ignored(session, tmp_path):
    ci._write(tmp_path / "c001.req", {"ts": 1})
    session._on_refresh(None, bytearray([0x02, 0x01]))
    session._on_refresh(None, bytearray([0x07]))
    assert not (tmp_path / "c001.ans").exists()
    assert not session.refresh_requested.is_set()
