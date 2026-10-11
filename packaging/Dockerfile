# Dockerfile - multi-stage build for nacho-moderator (SPEC-ADDENDUM-TOOLCHAIN A2).
#
# The runtime image is a slim Debian with only the shared libraries the bot
# needs. io_uring is a kernel feature: run the container with
#   --security-opt seccomp=unconfined
# (or a custom profile that allows io_uring_setup/io_uring_enter/io_uring_register),
# because Docker's default seccomp profile blocks those syscalls.

# --- Build stage -----------------------------------------------------------
FROM debian:bookworm AS build

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential cmake ninja-build pkg-config git ca-certificates \
        liburing-dev libssl-dev libsqlite3-dev libpcre2-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .
RUN git submodule update --init --recursive || true
RUN cmake --preset release && cmake --build --preset release

# --- Runtime stage ---------------------------------------------------------
FROM debian:bookworm-slim AS runtime

RUN apt-get update && apt-get install -y --no-install-recommends \
        liburing2 libssl3 libsqlite3-0 libpcre2-8-0 ca-certificates \
    && rm -rf /var/lib/apt/lists/* \
    && useradd --system --home /var/lib/nachomoderator --create-home nachomoderator

COPY --from=build /src/build/release/tgbot /usr/local/bin/tgbot
COPY --from=build /src/config/config.example.toml /etc/nachomoderator/config.toml

RUN chown -R nachomoderator:nachomoderator /var/lib/nachomoderator \
    && chmod 0700 /etc/nachomoderator && chmod 0600 /etc/nachomoderator/config.toml

USER nachomoderator
WORKDIR /var/lib/nachomoderator

# The bot has no listening port in polling mode; webhook mode listens per config.
ENTRYPOINT ["/usr/local/bin/tgbot"]
CMD ["--config", "/etc/nachomoderator/config.toml"]
