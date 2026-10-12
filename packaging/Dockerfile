# Dockerfile - multi-stage build for nacho-moderator (Python + uv).
#
# The runtime image is a slim Debian with only the project and its dependencies.

# --- Build stage -----------------------------------------------------------
FROM python:3.12-slim-bookworm AS build

COPY --from=ghcr.io/astral-sh/uv:0.12.13 /uv /uvx /bin/

WORKDIR /src
COPY pyproject.toml uv.lock README.md ./
COPY src ./src
RUN uv sync --frozen --no-dev --no-editable

# --- Runtime stage ---------------------------------------------------------
FROM python:3.12-slim-bookworm AS runtime

RUN useradd --system --home /var/lib/nachomoderator --create-home nachomoderator

WORKDIR /app
COPY --from=build /src/.venv /app/.venv
COPY --from=build /src/src /app/src
COPY config/config.example.toml /etc/nachomoderator/config.toml

ENV PATH="/app/.venv/bin:$PATH"

RUN chown -R nachomoderator:nachomoderator /var/lib/nachomoderator \
    && chmod 0700 /etc/nachomoderator && chmod 0600 /etc/nachomoderator/config.toml

USER nachomoderator
WORKDIR /var/lib/nachomoderator

# The bot has no listening port in polling mode; webhook mode listens per config.
ENTRYPOINT ["python", "-m", "nacho_moderator"]
CMD ["--config", "/etc/nachomoderator/config.toml"]
