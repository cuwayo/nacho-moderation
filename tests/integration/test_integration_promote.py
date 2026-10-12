"""End-to-end tests for /promote, /demote and /start (P4).

Each test runs the real ``nacho_moderator`` process against the scripted mock
Bot API server and asserts on the calls the mock recorded, so the full path
(config -> dispatcher -> middleware -> router -> handler -> Bot API) is
exercised.
"""

from __future__ import annotations

from collections.abc import Callable

from tests.integration.conftest import IntegrationRun


def test_promote_end_to_end(start_bot: Callable[..., IntegrationRun]) -> None:
    """An admin's /promote issues a limited promoteChatMember and sets the title."""
    run = start_bot("promote_reply.json")
    promote_call = run.wait_for("promoteChatMember")
    payload = promote_call.get("json") or {}
    assert payload.get("user_id") == 300
    assert payload.get("can_delete_messages") is True
    assert payload.get("can_restrict_members") is True
    assert payload.get("can_pin_messages") is True
    # The dangerous rights are never granted.
    assert payload.get("can_promote_members") in (False, "false")
    assert payload.get("can_change_info") in (False, "false")
    assert payload.get("can_manage_chat") in (False, "false")

    title_call = run.wait_for("setChatAdministratorCustomTitle")
    assert (title_call.get("json") or {}).get("custom_title") == "Moderator"


def test_demote_end_to_end(start_bot: Callable[..., IntegrationRun]) -> None:
    """The owner's /demote on an admin clears every right."""
    run = start_bot("demote_admin.json")
    call = run.wait_for("promoteChatMember")
    payload = call.get("json") or {}
    assert payload.get("user_id") == 200
    for right in (
        "can_delete_messages",
        "can_restrict_members",
        "can_pin_messages",
        "can_promote_members",
        "can_change_info",
        "can_manage_chat",
    ):
        assert payload.get(right) in (False, "false"), right


def test_start_end_to_end_in_private(start_bot: Callable[..., IntegrationRun]) -> None:
    """/start from a plain member in a private chat replies with the help text."""
    run = start_bot("start_private.json")
    send = run.wait_for("sendMessage")
    text = (send.get("json") or {}).get("text", "")
    assert "Available commands" in text
    # No moderation call was made.
    assert "banChatMember" not in run.methods()
    assert "promoteChatMember" not in run.methods()
