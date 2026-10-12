# Dockerfile - multi-stage build for nacho-moderator (Python + uv).
#
# The runtime image is a slim Debian with only the project and its dependencies.
# Base images are pinned by digest so a rebuild is reproducible; bump the digest
# and the tag together (Dependabot tracks the digest).

# --- Build stage -----------------------------------------------------------
# python:3.12-slim-bookworm
FROM python:3.12-slim-bookworm@sha256:34386ef0cb081344d7ec1c103ba398e6e9f64e9ab3a1509accc92a4e24a07258 AS build

COPY --from=ghcr.io/astral-sh/uv:0.12.13 /uv /uvx /bin/

WORKDIR /src
COPY pyproject.toml uv.lock README.md ./
COPY src ./src
RUN uv sync --frozen --no-dev --no-editable

# --- Runtime stage ---------------------------------------------------------
# python:3.12-slim-bookworm
FROM python:3.12-slim-bookworm@sha256:34386ef0cb081344d7ec1c103ba398e6e9f64e9ab3a1509accc92a4e24a07258 AS runtime

# A non-root, dedicated user. The bot needs no privileges.
RUN useradd --system --home /var/lib/nachomoderator --create-home --shell /usr/sbin/nologin nachomoderator

WORKDIR /app
COPY --from=build /src/.venv /app/.venv
COPY --from=build /src/src /app/src
COPY config/config.example.toml /etc/nachomoderator/config.toml

ENV PATH="/app/.venv/bin:$PATH" \
    PYTHONUNBUFFERED=1 \
    PYTHONDONTWRITEBYTECODE=1

RUN chown -R nachomoderator:nachomoderator /var/lib/nachomoderator \
    && chmod 0700 /etc/nachomoderator && chmod 0600 /etc/nachomoderator/config.toml

USER nachomoderator
WORKDIR /var/lib/nachomoderator

# Read-only-root-FS compatible: the only writable path is the state dir. A
# container healthcheck runs `--check-config`, which exercises config load and
# validation without starting the bot.
HEALTHCHECK --interval=30s --timeout=10s --start-period=10s --retries=3 \
    CMD ["python", "-m", "nacho_moderator", "--check-config", "/etc/nachomoderator/config.toml"]

# The bot has no listening port in polling mode; webhook mode listens per config.
ENTRYPOINT ["python", "-m", "nacho_moderator"]
CMD ["--config", "/etc/nachomoderator/config.toml"]
