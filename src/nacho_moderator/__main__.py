"""Allow ``python -m nacho_moderator`` to run the bot."""

from __future__ import annotations

from nacho_moderator.main import main

if __name__ == "__main__":
    raise SystemExit(main())
