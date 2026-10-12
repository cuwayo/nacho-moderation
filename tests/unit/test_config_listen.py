"""Tests for the webhook listen parser (P3).

``webhook.listen`` accepts ``host:port`` and ``[v6]:port`` and returns the
*unbracketed* host so it can be bound directly. A bare port, an empty host, an
unbracketed IPv6 address, and an out-of-range port are all rejected at config
load time, so ``--check-config`` catches them.
"""

from __future__ import annotations

import socket

import pytest
from pydantic import ValidationError

from nacho_moderator.config import WebhookSettings, load_settings, parse_listen
from nacho_moderator.errors import ConfigError


@pytest.mark.parametrize(
    ("value", "expected"),
    [
        ("127.0.0.1:8080", ("127.0.0.1", 8080)),
        ("0.0.0.0:80", ("0.0.0.0", 80)),
        ("example.com:443", ("example.com", 443)),
        ("localhost:1", ("localhost", 1)),
        ("[::1]:8080", ("::1", 8080)),
        ("[::]:8080", ("::", 8080)),
        ("[2001:db8::1]:65535", ("2001:db8::1", 65535)),
        ("  127.0.0.1:8080  ", ("127.0.0.1", 8080)),
    ],
)
def test_parse_listen_accepts(value: str, expected: tuple[str, int]) -> None:
    assert parse_listen(value) == expected


@pytest.mark.parametrize(
    "value",
    [
        "",
        "   ",
        "8080",  # bare port
        ":8080",  # empty host
        "127.0.0.1:",  # empty port
        "127.0.0.1:abc",  # non-numeric port
        "127.0.0.1:0",  # port below range
        "127.0.0.1:65536",  # port above range
        "[::1]8080",  # missing colon after bracket
        "[::1:8080",  # missing closing bracket
        "::1:8080",  # unbracketed IPv6 is ambiguous
    ],
)
def test_parse_listen_rejects(value: str) -> None:
    with pytest.raises(ValueError, match=r"webhook\.listen"):
        parse_listen(value)


def test_webhook_settings_host_is_unbracketed() -> None:
    settings = WebhookSettings(listen="[::1]:8080")
    assert settings.host == "::1"
    assert settings.port == 8080


def test_webhook_settings_rejects_bare_port() -> None:
    with pytest.raises(ValidationError):
        WebhookSettings(listen="8080")


def test_check_config_catches_bad_listen(tmp_path: object) -> None:
    from pathlib import Path

    path = Path(str(tmp_path)) / "config.toml"
    path.write_text(
        'bot_token = "1:A"\nmode = "webhook"\n'
        '[webhook]\nlisten = "8080"\nsecret = "0123456789abcdef"\n',
        encoding="utf-8",
    )
    path.chmod(0o600)
    with pytest.raises(ConfigError) as excinfo:
        load_settings(path, environ={})
    assert "webhook.listen" in str(excinfo.value)


def test_check_config_accepts_ipv6_listen(tmp_path: object) -> None:
    from pathlib import Path

    path = Path(str(tmp_path)) / "config.toml"
    path.write_text(
        'bot_token = "1:A"\nmode = "webhook"\n'
        '[webhook]\nlisten = "[::1]:8080"\nsecret = "0123456789abcdef"\n',
        encoding="utf-8",
    )
    path.chmod(0o600)
    settings = load_settings(path, environ={})
    assert settings.webhook.host == "::1"
    assert settings.webhook.port == 8080


def _ipv6_available() -> bool:
    """Return whether this host can bind an IPv6 loopback socket."""
    if not socket.has_ipv6:
        return False
    try:
        with socket.socket(socket.AF_INET6, socket.SOCK_STREAM) as sock:
            sock.bind(("::1", 0))
    except OSError:
        return False
    return True


def test_real_bind_ipv4() -> None:
    """The parsed IPv4 host binds for real."""
    host, _ = parse_listen("127.0.0.1:8080")
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind((host, 0))
        sock.listen(1)
        assert sock.getsockname()[0] == "127.0.0.1"


def test_real_bind_ipv6() -> None:
    """The parsed (unbracketed) IPv6 host binds for real."""
    if not _ipv6_available():
        pytest.skip("host has no IPv6 loopback")
    host, _ = parse_listen("[::1]:8080")
    with socket.socket(socket.AF_INET6, socket.SOCK_STREAM) as sock:
        sock.bind((host, 0))
        sock.listen(1)
        assert sock.getsockname()[0] == "::1"


async def test_aiohttp_binds_parsed_host() -> None:
    """aiohttp (the real consumer) can bind the parsed host on both families."""
    from aiohttp import web

    for raw in ("127.0.0.1:8080", "[::1]:8080"):
        host, _ = parse_listen(raw)
        if ":" in host and not _ipv6_available():
            continue
        runner = web.AppRunner(web.Application())
        await runner.setup()
        site = web.TCPSite(runner, host, 0)
        try:
            await site.start()
            assert site._server is not None  # bound
        finally:
            await runner.cleanup()
