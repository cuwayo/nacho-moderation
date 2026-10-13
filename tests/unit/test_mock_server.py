"""Tests for the soak-mode behaviour of the mock Bot API server.

The mock's ``MockState`` is a plain object, so its batching and backlog logic can
be driven directly without starting the HTTP server. These cover the F2 fix: the
soak serves up to ``soak_batch`` updates per ``getUpdates`` (Telegram's default
100) so the achieved rate tracks the requested rate, and the pending queue is
bounded so a slow bot cannot grow it without limit.
"""

from __future__ import annotations

from tests.mock import mock_tg_server
from tests.mock.mock_tg_server import SOAK_BATCH_DEFAULT, MockState


def _soak_state(*, batch: int, backlog: int) -> MockState:
    state = MockState()
    state.soak = True
    state.soak_rate = 1000
    state.soak_batch = batch
    state.soak_max_backlog = backlog
    return state


def test_default_batch_is_telegrams_hundred() -> None:
    assert SOAK_BATCH_DEFAULT == 100
    assert MockState().soak_batch == 100


def test_get_updates_serves_up_to_the_batch_size() -> None:
    state = _soak_state(batch=100, backlog=10_000)
    for _ in range(250):
        state._enqueue_soak_update()
    first = state.next_update_batch()["result"]
    second = state.next_update_batch()["result"]
    third = state.next_update_batch()["result"]
    assert len(first) == 100
    assert len(second) == 100
    assert len(third) == 50
    # update_ids are unique and increasing across batches.
    ids = [u["update_id"] for u in first + second + third]
    assert ids == sorted(ids)
    assert len(set(ids)) == 250


def test_batch_of_one_is_available_for_legacy_runs() -> None:
    state = _soak_state(batch=1, backlog=10_000)
    for _ in range(3):
        state._enqueue_soak_update()
    assert len(state.next_update_batch()["result"]) == 1


def test_empty_when_no_updates_queued() -> None:
    state = _soak_state(batch=100, backlog=10_000)
    assert state.next_update_batch()["result"] == []


def test_backlog_is_capped_and_oldest_dropped(capsys) -> None:
    """A slow bot cannot grow the queue without bound; the oldest are dropped."""
    state = _soak_state(batch=100, backlog=500)
    for _ in range(800):
        state._enqueue_soak_update()
    assert len(state.soak_batches) == 500
    assert state.soak_dropped == 300
    # The surviving updates are the newest ones (id 301..800).
    assert state.soak_batches[0]["update_id"] == 301
    assert state.soak_batches[-1]["update_id"] == 800
    assert "soak backlog" in capsys.readouterr().err


def test_unbounded_backlog_does_not_drop() -> None:
    state = _soak_state(batch=100, backlog=0)  # 0 = unbounded
    for _ in range(1000):
        state._enqueue_soak_update()
    assert len(state.soak_batches) == 1000
    assert state.soak_dropped == 0


def test_feeder_waits_for_the_first_poll() -> None:
    """The feeder blocks until the bot's first getUpdates (no startup backlog)."""
    state = _soak_state(batch=100, backlog=10_000)
    # Not started: the feeder's wait would block, so drive the event instead.
    assert not state.soak_started.is_set()
    state.next_update_batch()
    assert state.soak_started.is_set()


def test_scripted_updates_still_win_over_soak() -> None:
    state = _soak_state(batch=100, backlog=10_000)
    state.scripted = [[{"update_id": 999}]]
    assert state.next_update_batch()["result"] == [{"update_id": 999}]


def test_backlog_cap_is_five_seconds_at_the_rate() -> None:
    """The cap the CLI derives is ``rate * SOAK_MAX_BACKLOG_SECONDS``."""
    assert mock_tg_server.SOAK_MAX_BACKLOG_SECONDS == 5.0
    assert max(1, int(1000 * mock_tg_server.SOAK_MAX_BACKLOG_SECONDS)) == 5000
    assert max(1, int(250 * mock_tg_server.SOAK_MAX_BACKLOG_SECONDS)) == 1250
