"""Welcome-message rendering.

The template supports four placeholders: ``{first}``, ``{mention}``, ``{chat}``
and ``{count}``. ``{mention}`` expands to an HTML anchor (raw markup); every
other substituted value is HTML-escaped, and unknown ``{...}`` tokens pass
through unchanged.
"""

from __future__ import annotations

from aiogram.types import User

from nacho_moderator.models import html_escape


def render_welcome(
    template: str,
    user: User | None,
    chat_title: str,
    member_count: int,
) -> str:
    """Render a welcome template against a joining user and chat.

    Args:
        template: The stored template text.
        user: The joining user, if known.
        chat_title: The chat title.
        member_count: The chat member count, or 0 when unknown.

    Returns:
        The rendered text with ``parse_mode=HTML`` markup.
    """
    first = user.first_name if user and user.first_name else "there"
    username = user.username if user else None

    if username:
        mention = f'<a href="https://t.me/{username}">{html_escape(first)}</a>'
    else:
        mention = html_escape(first)

    replacements = {
        "first": html_escape(first),
        "mention": mention,
        "chat": html_escape(chat_title),
        "count": str(member_count),
    }

    out: list[str] = []
    i = 0
    while i < len(template):
        if template[i] == "{":
            end = template.find("}", i + 1)
            if end != -1:
                key = template[i + 1 : end]
                if key in replacements:
                    out.append(replacements[key])
                    i = end + 1
                    continue
        out.append(template[i])
        i += 1
    return "".join(out)
