"""Tests for secret redaction in logging (P1).

Redaction must hold on the *final* rendered string, so these tests drive a real
``logging`` handler and inspect what it emitted. A property test places random
secrets in every position a log call can put one — the message, args (tuple and
mapping), exception text, chained exceptions, ``exc_info`` and ``stack_info`` —
and asserts the raw value never survives.
"""

from __future__ import annotations

import logging

import pytest
from hypothesis import given, settings
from hypothesis import strategies as st

from nacho_moderator.config import Settings
from nacho_moderator.logging_setup import (
    RedactingFormatter,
    configure_logging,
    register_secret,
    reset_secrets,
)

# Tokens are "<digits>:<base64-ish>"; the tail is long enough to look real.
_TOKEN = "1234567890:AAHsecretTOKENvalue0123456789abcde"
_SECRET = "a-long-webhook-secret-value"
_SECRET_TAIL = _TOKEN.split(":", 1)[1]


@pytest.fixture(autouse=True)
def _clean_secrets() -> None:
    """Start each test with no registered secrets."""
    reset_secrets()


class _Capture:
    """A root handler that records the formatted output of every record."""

    def __init__(self) -> None:
        self.records: list[str] = []
        self.handler = logging.StreamHandler(self)
        self.handler.setFormatter(RedactingFormatter("%(levelname)s %(name)s %(message)s"))

    def write(self, text: str) -> None:
        if text.strip():
            self.records.append(text)

    def flush(self) -> None:
        return

    @property
    def output(self) -> str:
        return "".join(self.records)


@pytest.fixture
def capture(monkeypatch: pytest.MonkeyPatch) -> _Capture:
    """Route the root logger into an in-memory capturing handler."""
    cap = _Capture()
    root = logging.getLogger()
    monkeypatch.setattr(root, "handlers", [cap.handler])
    root.setLevel(logging.DEBUG)
    return cap


def test_registered_token_redacted_in_plain_message(capture: _Capture) -> None:
    register_secret(_TOKEN)
    logging.getLogger("t").info("token is %s", _TOKEN)
    assert _TOKEN not in capture.output
    assert "<redacted>" in capture.output


def test_registered_token_redacted_without_bot_prefix(capture: _Capture) -> None:
    """Leak 3: a bare token with no ``bot`` prefix."""
    register_secret(_TOKEN)
    logging.getLogger("t").warning("using %s", _TOKEN)
    assert _TOKEN not in capture.output


def test_url_assembled_at_render_time(capture: _Capture) -> None:
    """Leak 1: the ``bot`` prefix lives in the format string, not the arg."""
    register_secret(_TOKEN)
    logging.getLogger("t").info("https://api.telegram.org/bot%s/getMe", _TOKEN)
    out = capture.output
    assert _TOKEN not in out
    assert _SECRET_TAIL not in out


def test_token_in_url_never_registered(capture: _Capture) -> None:
    """A token-shaped value is caught by pattern even without registration."""
    logging.getLogger("t").info("GET /bot%s/getMe", _TOKEN)
    assert _TOKEN not in capture.output


def test_bare_token_shape_without_bot_prefix(capture: _Capture) -> None:
    logging.getLogger("t").info("token=%s", _TOKEN)
    assert _TOKEN not in capture.output


def test_webhook_secret_redacted(capture: _Capture) -> None:
    """Leak 4: the webhook secret was never redacted before."""
    register_secret(_SECRET)
    logging.getLogger("t").info("secret header %s", _SECRET)
    assert _SECRET not in capture.output


def test_exception_text_redacted(capture: _Capture) -> None:
    """Leak 2: a traceback must be redacted too."""
    register_secret(_TOKEN)
    try:
        raise RuntimeError(f"boom {_TOKEN}")
    except RuntimeError:
        logging.getLogger("t").exception("failed")
    out = capture.output
    assert _TOKEN not in out
    assert _SECRET_TAIL not in out


def test_chained_exception_redacted(capture: _Capture) -> None:
    register_secret(_TOKEN)
    try:
        try:
            raise ValueError(f"inner {_TOKEN}")
        except ValueError as exc:
            raise RuntimeError("outer") from exc
    except RuntimeError:
        logging.getLogger("t").exception("failed")
    assert _TOKEN not in capture.output


def test_exc_info_true_redacted(capture: _Capture) -> None:
    register_secret(_TOKEN)
    try:
        raise RuntimeError(f"boom {_TOKEN}")
    except RuntimeError:
        logging.getLogger("t").error("failed", exc_info=True)
    assert _TOKEN not in capture.output


def test_stack_info_redacted(capture: _Capture) -> None:
    register_secret(_TOKEN)
    logging.getLogger("t").info("here %s", _TOKEN, stack_info=True)
    out = capture.output
    assert _TOKEN not in out
    assert "Stack (most recent call last)" in out


def test_dict_args_not_corrupted(capture: _Capture) -> None:
    """The old filter iterated a mapping argument and printed ``x a``."""
    logging.getLogger("t").info("x %s", {"a": 1})
    assert capture.output.rstrip().endswith("x {'a': 1}")


def test_mapping_args_redacted(capture: _Capture) -> None:
    register_secret(_TOKEN)
    logging.getLogger("t").info("x %(v)s", {"v": _TOKEN})
    assert _TOKEN not in capture.output


def test_tuple_args_redacted(capture: _Capture) -> None:
    register_secret(_TOKEN)
    logging.getLogger("t").info("%s and %s", _TOKEN, "safe")
    assert _TOKEN not in capture.output
    assert "safe" in capture.output


def test_registered_secret_masks_url_quoted_form(capture: _Capture) -> None:
    register_secret(_TOKEN)
    from urllib.parse import quote

    logging.getLogger("t").info("body=%s", quote(_TOKEN, safe=""))
    assert quote(_TOKEN, safe="") not in capture.output


def test_secret_tail_after_colon_redacted(capture: _Capture) -> None:
    register_secret(_TOKEN)
    logging.getLogger("t").info("tail=%s", _SECRET_TAIL)
    assert _SECRET_TAIL not in capture.output


def test_short_secret_is_ignored(capture: _Capture) -> None:
    """A value shorter than 8 characters is not registered (too broad)."""
    register_secret("abc")
    logging.getLogger("t").info("abc def abc")
    assert "abc" in capture.output


@pytest.mark.parametrize("logger_name", ["aiogram", "aiohttp", "asyncio"])
def test_third_party_loggers_are_redacted(capture: _Capture, logger_name: str) -> None:
    """aiogram/aiohttp/asyncio propagate to the root handler and are redacted."""
    register_secret(_TOKEN)
    logging.getLogger(logger_name).warning("request to /bot%s/getMe", _TOKEN)
    assert _TOKEN not in capture.output


def test_configure_logging_installs_redacting_formatter(
    capsys: pytest.CaptureFixture[str],
) -> None:
    from nacho_moderator.config import LogLevelName

    # configure_logging mutates the global root logger; restore it afterwards so
    # the handler bound to capsys's (soon-closed) stderr does not linger and
    # break a later background thread that logs through the root logger.
    root = logging.getLogger()
    saved_handlers = root.handlers[:]
    saved_level = root.level
    try:
        register_secret(_TOKEN)
        configure_logging(LogLevelName.INFO)
        logging.getLogger("nacho_moderator.test").info("token %s", _TOKEN)
        err = capsys.readouterr().err
        assert _TOKEN not in err
        assert "<redacted>" in err
    finally:
        root.handlers[:] = saved_handlers
        root.setLevel(saved_level)


def test_settings_repr_masks_secrets() -> None:
    settings = Settings.model_validate(
        {"bot_token": _TOKEN, "webhook": {"secret": _SECRET}, "mode": "polling"}
    )
    assert _TOKEN not in repr(settings)
    assert _SECRET not in repr(settings)
    assert _TOKEN not in str(settings)
    assert _SECRET not in str(settings)
    assert _TOKEN not in f"{settings}"
    assert "***" in repr(settings)


def test_webhook_settings_repr_masks_secret() -> None:
    from nacho_moderator.config import WebhookSettings

    sub = WebhookSettings(secret=_SECRET)
    assert _SECRET not in repr(sub)
    assert _SECRET not in str(sub)


def test_validation_error_does_not_echo_token(tmp_path: object) -> None:
    """A failed validation must not print the token it was given."""
    from pathlib import Path

    from nacho_moderator.config import load_settings
    from nacho_moderator.errors import ConfigError

    path = Path(str(tmp_path)) / "config.toml"
    path.write_text(
        f'bot_token = "{_TOKEN}"\nmode = "webhook"\n[webhook]\nsecret = "short"\n',
        encoding="utf-8",
    )
    with pytest.raises(ConfigError) as excinfo:
        load_settings(path, environ={})
    message = str(excinfo.value)
    assert _TOKEN not in message
    assert _SECRET_TAIL not in message


# --- Property test ---------------------------------------------------------

_SECRET_STRATEGY = st.one_of(
    st.text(
        alphabet="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-",
        min_size=30,
        max_size=45,
    ),
    st.from_regex(r"\d{6,12}:[A-Za-z0-9_-]{30,40}", fullmatch=True),
)


@st.composite
def _log_call(draw: st.DrawFn) -> tuple[str, str]:
    """Build ``(log_call_kind, secret)``.

    The secret is placed in a random position: the message, a tuple arg, a
    mapping arg, exception text, a chained exception, ``exc_info``, or a URL
    assembled at render time.
    """
    secret = draw(_SECRET_STRATEGY)
    kind = draw(
        st.sampled_from(
            ["message", "tuple_arg", "dict_arg", "exception", "chained", "exc_info", "url"]
        )
    )
    return kind, secret


def _render_log(kind: str, secret: str) -> str:
    """Emit one log call of ``kind`` and return the formatted output."""
    import io

    stream = io.StringIO()
    handler = logging.StreamHandler(stream)
    handler.setFormatter(RedactingFormatter("%(levelname)s %(name)s %(message)s"))
    logger = logging.getLogger(f"prop.{kind}")
    logger.handlers = [handler]
    logger.propagate = False
    logger.setLevel(logging.DEBUG)

    if kind == "message":
        logger.info("prefix %s suffix", secret)
    elif kind == "tuple_arg":
        logger.info("%s %s", secret, "x")
    elif kind == "dict_arg":
        logger.info("%(s)s", {"s": secret})
    elif kind == "exception":
        try:
            raise RuntimeError(f"boom {secret}")
        except RuntimeError:
            logger.exception("failed")
    elif kind == "chained":
        try:
            try:
                raise ValueError(f"inner {secret}")
            except ValueError as exc:
                raise RuntimeError("outer") from exc
        except RuntimeError:
            logger.exception("failed")
    elif kind == "exc_info":
        try:
            raise RuntimeError(f"boom {secret}")
        except RuntimeError:
            logger.error("failed", exc_info=True)
    elif kind == "url":
        logger.info("https://api.telegram.org/bot%s/getMe", secret)
    return stream.getvalue()


@given(call=_log_call())
@settings(max_examples=200, deadline=None)
def test_property_secret_never_survives_in_output(call: tuple[str, str]) -> None:
    """For random secrets in random positions, the raw value never appears."""
    kind, secret = call
    reset_secrets()
    register_secret(secret)
    output = _render_log(kind, secret)
    assert secret not in output
