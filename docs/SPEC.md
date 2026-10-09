# SPEC: Telegram Group Admin Bot (C + liburing)

## 1. Goal
Build a Telegram bot in **C (C11/C17)** that moderates and manages groups/supergroups.
All network I/O must be **asynchronous via `liburing`** (io_uring) in a **single-threaded event loop**. Blocking work (SQLite, DNS) is isolated in worker threads that signal the loop through an `eventfd`.

## 2. Non-Goals
- No GUI or web dashboard.
- No ML-based spam detection.
- No multi-process clustering (single process; scale later by sharding on `chat_id`).
- No support for Windows/macOS (Linux kernel >= 5.15 only).

## 3. Tech Stack & Constraints
| Concern | Decision |
|---|---|
| Language | C11, `-Wall -Wextra -Werror -fsanitize=address,undefined` in debug |
| Async I/O | `liburing` (>= 2.3). Only use io_uring for sockets, timers, eventfd. No epoll/poll fallback. |
| TLS | OpenSSL 3 with **memory BIOs** (`BIO_s_mem`) driven manually by io_uring `recv`/`send`. Do NOT use blocking `SSL_read/SSL_write` on the socket fd. |
| HTTP | Hand-written minimal HTTP/1.1 client (keep-alive, chunked decoding, Content-Length). Optional: embedded HTTP/1.1 server for webhook. |
| JSON | `yyjson` (preferred) or `cJSON` |
| Storage | SQLite3 (WAL mode) accessed ONLY from a dedicated DB worker thread |
| DNS | `getaddrinfo` in a worker thread at startup + refresh every 5 min; cache result |
| Build | CMake or Makefile, out-of-tree build |
| Logging | Structured line logs to stderr, levels: ERROR/WARN/INFO/DEBUG |

## 4. Architecture

```
            +-----------------------------+
            |        io_uring loop        |  (main thread)
            |  CQE dispatcher by op->type |
            +--+---------+----------+-----+
               |         |          |
        +------v--+ +----v-----+ +--v--------+
        | Updates | | API      | | Timers    |
        | source  | | client   | | (timeout) |
        | (poll / | | (TLS+HTTP| |           |
        | webhook)| |  pool)   | |           |
        +----+----+ +----^-----+ +-----------+
             |           |
        +----v-----------+-----+      eventfd      +-----------------+
        |  Router / Handlers   | <----------------> | DB worker thread|
        |  (commands, events)  |   request/response | (SQLite)        |
        +----------------------+      queues        +-----------------+
```

### 4.1 Update Source (choose via config `mode`)
1. **`polling`** (default): long-poll `getUpdates` (`timeout=30`, `allowed_updates` set explicitly). One persistent TLS connection dedicated to polling. Track `offset = last_update_id + 1`.
2. **`webhook`**: bot listens on plain HTTP behind a reverse proxy (nginx/caddy) that terminates TLS. Accept loop uses `io_uring_prep_accept` (multishot if available). Verify header `X-Telegram-Bot-Api-Secret-Token`.

Required `allowed_updates`: `message`, `edited_message`, `callback_query`, `chat_member`, `my_chat_member`.

### 4.2 API Client
- Connection pool (default 4 TLS connections to `api.telegram.org:443`), HTTP keep-alive, reconnect on error.
- Each API call = one `api_request_t` with: method name, JSON body, callback `void (*cb)(api_result_t*, void *ctx)`, retry count, deadline.
- Outbound **rate limiter** (token buckets): global 25 req/s, per-chat 15 req/min for messages, per-chat 1 req/s for bulk ops. Excess requests are queued, never dropped silently.
- On HTTP 429: read `parameters.retry_after`, pause that bucket via `io_uring_prep_timeout`, then retry.
- On 5xx / network error: exponential backoff (1s, 2s, 4s, max 30s), max 5 retries for idempotent calls.

### 4.3 io_uring Op Context
Every submitted SQE carries a pointer in `user_data` to:
```c
typedef enum { OP_ACCEPT, OP_RECV, OP_SEND, OP_CONNECT, OP_TIMEOUT,
               OP_EVENTFD, OP_CLOSE } op_type_t;

typedef struct op {
    op_type_t type;
    void     *owner;            // conn_t*, timer_t*, etc.
    void     (*on_complete)(struct op *, int res);
} op_t;
```
Rules:
- Ring init with `IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN` when supported, fallback to defaults.
- Batch submissions (`io_uring_submit` once per loop iteration).
- Handle `-EINTR`, `-EAGAIN`, `-ECANCELED`, short reads/writes.
- Use `IORING_OP_LINK_TIMEOUT` for connect/read deadlines.
- Registered buffers / provided buffer rings (`io_uring_setup_buf_ring`) for recv are recommended.
- No operation may block the loop for > 5 ms.

### 4.4 DB Worker
- MPSC queue (mutex+cond or lock-free ring) of `db_job_t { sql_op, params, cb, ctx }`.
- On completion, worker pushes result to a response queue and writes to the `eventfd`; loop reads it via `io_uring_prep_read` and dispatches callbacks on the main thread.
- SQLite pragmas: `journal_mode=WAL`, `synchronous=NORMAL`, `foreign_keys=ON`, `busy_timeout=5000`.
- Hot data (admin cache, settings, flood counters) lives in an in-memory hash table with TTL; DB is the source of truth for warns/bans/settings.

## 5. Features

### 5.1 Permission Model
- Roles: `owner` (Telegram creator), `admin` (Telegram admin), `member`, plus bot-level `sudo` users from config.
- Cache `getChatAdministrators` per chat (TTL 10 min; invalidate on `chat_member` update and on `/reload`).
- Each command declares required role + required Telegram right (e.g. `can_restrict_members`). Check BOTH caller rights and bot rights; reply with a clear error if the bot lacks rights.
- Never allow acting on owner/admins/bot itself (unless caller is owner/sudo for demote).

### 5.2 Commands
Target resolution order: reply-to message user, then `@username` (resolved from DB cache), then numeric user_id.
Optional duration suffix: `30s`, `10m`, `2h`, `7d`, `1w`. No duration = permanent.
Optional trailing text = reason.

| Command | Description | Telegram API | Required right |
|---|---|---|---|
| `/ban [target] [dur] [reason]` | Ban user | `banChatMember` | can_restrict_members |
| `/unban [target]` | Unban | `unbanChatMember` (`only_if_banned=true`) | can_restrict_members |
| `/kick [target]` | Remove, allow rejoin | `banChatMember` then `unbanChatMember` | can_restrict_members |
| `/mute [target] [dur] [reason]` | Restrict sending | `restrictChatMember` | can_restrict_members |
| `/unmute [target]` | Restore permissions | `restrictChatMember` | can_restrict_members |
| `/warn [target] [reason]` | Add warning; auto-action at limit | DB + action | can_restrict_members |
| `/unwarn [target]` | Remove last warning | DB | can_restrict_members |
| `/warns [target]` | List warnings | DB | any admin |
| `/resetwarns [target]` | Clear warnings | DB | any admin |
| `/promote [target] [title]` | Promote with limited rights | `promoteChatMember` | can_promote_members |
| `/demote [target]` | Remove admin | `promoteChatMember` (all false) | can_promote_members |
| `/pin` (reply) / `/unpin` | Pin/unpin | `pinChatMessage` / `unpinChatMessage` | can_pin_messages |
| `/del` (reply) | Delete message + command | `deleteMessage` | can_delete_messages |
| `/purge` (reply) | Delete from replied message to now | `deleteMessages` in batches of 100 | can_delete_messages |
| `/lock <type>` / `/unlock <type>` | Lock links, media, stickers, forwards, etc. | DB + auto-delete | can_delete_messages |
| `/setwelcome <text>` / `/welcome on\|off` | Welcome message with placeholders `{first}`, `{mention}`, `{chat}`, `{count}` | `sendMessage` | can_change_info |
| `/setrules <text>` / `/rules` | Store/show rules | `sendMessage` | can_change_info |
| `/blacklist add\|del\|list <word>` | Word/regex blacklist | DB | can_delete_messages |
| `/antiflood <n> <secs>` / `off` | Flood control | DB | can_restrict_members |
| `/setwarnlimit <n>` / `/setwarnaction ban\|mute\|kick` | Warn policy | DB | can_restrict_members |
| `/captcha on\|off` | Join verification | DB | can_restrict_members |
| `/setlog <chat_id>` | Moderation log channel | DB | owner |
| `/reload` | Refresh admin cache | `getChatAdministrators` | any admin |
| `/settings` | Inline-keyboard settings panel | `sendMessage` + callbacks | can_change_info |
| `/help`, `/start` | Help text | `sendMessage` | anyone |

Registering: call `setMyCommands` with scopes `all_group_chats` and `all_chat_administrators` at startup.

### 5.3 Automated Moderation (non-command)
Order of processing for every group message:
1. Ignore bots / service messages except join/leave handling.
2. Skip moderation for admins and `sudo`.
3. **Locks** (link/media/etc.) -> delete + optional warn.
4. **Blacklist** match -> delete + warn/mute per config.
5. **Antiflood**: sliding window counter per `(chat_id, user_id)` in memory; on exceed -> mute N minutes, delete burst messages.
6. Update message statistics (optional).

### 5.4 Join / Captcha Flow
- Handle `chat_member` (preferred) and `message.new_chat_members`.
- If captcha on: immediately `restrictChatMember` (no send), send inline-keyboard button "I'm human", store pending row with deadline.
- Callback from the correct user -> lift restriction, delete captcha message, send welcome.
- Timeout (default 120 s, via io_uring timer or periodic sweep) -> `kick` and delete captcha message.
- Callback data format: `cap:<chat_id>:<user_id>:<nonce>` (max 64 bytes). Reject mismatching user_id.

### 5.5 Moderation Log
Every moderation action writes an `actions` row and, if `log_chat_id` is set, posts: action, target, admin, reason, duration, timestamp.

### 5.6 Expiring Actions
Temp bans/mutes are stored with `expires_at`. A periodic timer (every 30 s) asks the DB worker for expired rows and lifts them (`unbanChatMember` / restore permissions). Must survive restarts.

## 6. Data Model (SQLite)
```sql
CREATE TABLE chats (
  chat_id INTEGER PRIMARY KEY,
  title TEXT, type TEXT,
  welcome_enabled INTEGER DEFAULT 0, welcome_text TEXT,
  rules_text TEXT,
  captcha_enabled INTEGER DEFAULT 0, captcha_timeout INTEGER DEFAULT 120,
  warn_limit INTEGER DEFAULT 3, warn_action TEXT DEFAULT 'mute',
  flood_limit INTEGER DEFAULT 0, flood_window INTEGER DEFAULT 10,
  locks INTEGER DEFAULT 0,            -- bitmask
  log_chat_id INTEGER, lang TEXT DEFAULT 'en'
);
CREATE TABLE users (
  user_id INTEGER PRIMARY KEY, username TEXT COLLATE NOCASE,
  first_name TEXT, last_seen INTEGER
);
CREATE INDEX idx_users_username ON users(username);
CREATE TABLE warns (
  id INTEGER PRIMARY KEY, chat_id INTEGER, user_id INTEGER,
  admin_id INTEGER, reason TEXT, created_at INTEGER
);
CREATE INDEX idx_warns ON warns(chat_id, user_id);
CREATE TABLE actions (
  id INTEGER PRIMARY KEY, chat_id INTEGER, user_id INTEGER, admin_id INTEGER,
  action TEXT, reason TEXT, created_at INTEGER, expires_at INTEGER, lifted INTEGER DEFAULT 0
);
CREATE INDEX idx_actions_exp ON actions(expires_at) WHERE lifted=0 AND expires_at IS NOT NULL;
CREATE TABLE blacklist (
  chat_id INTEGER, pattern TEXT, is_regex INTEGER DEFAULT 0,
  PRIMARY KEY (chat_id, pattern)
);
CREATE TABLE pending_captcha (
  chat_id INTEGER, user_id INTEGER, message_id INTEGER,
  nonce TEXT, deadline INTEGER, PRIMARY KEY (chat_id, user_id)
);
CREATE TABLE schema_version (v INTEGER);
```
Include a migration mechanism driven by `schema_version`.

## 7. Configuration (`config.toml` or env vars)
```
BOT_TOKEN=...                 # required, never logged
MODE=polling|webhook
WEBHOOK_LISTEN=127.0.0.1:8080
WEBHOOK_SECRET=...
DB_PATH=./bot.db
SUDO_IDS=12345,67890
API_POOL_SIZE=4
LOG_LEVEL=info
```
Optionally support a custom `api_base` (self-hosted Telegram Bot API server over plain HTTP) to skip TLS for local deployments.

## 8. Source Layout
```
/src
  main.c            // init, signal handling (signalfd via io_uring), shutdown
  loop.c/.h         // io_uring setup, CQE dispatch, timers
  net_conn.c/.h     // TCP connection state machine
  tls.c/.h          // OpenSSL memory-BIO adapter
  http.c/.h         // request builder, response parser (chunked, keep-alive)
  tg_api.c/.h       // API client, rate limiter, retry, method wrappers
  tg_types.c/.h     // parsed Update/Message/User/Chat structs
  router.c/.h       // update -> handler dispatch
  cmd_*.c           // one file per command group (ban, warn, lock, ...)
  automod.c/.h      // flood, blacklist, locks
  captcha.c/.h
  perms.c/.h        // admin cache and permission checks
  db.c/.h           // SQLite worker thread + job queue
  cache.c/.h        // hash map with TTL
  util.c/.h         // arena allocator, string builder, duration parser, logging
/tests
CMakeLists.txt
```

## 9. Memory & Error Handling
- Per-update **arena allocator**; free everything when the handler chain completes.
- Every `malloc` checked; no unbounded buffers: max HTTP response 1 MiB, max update body 1 MiB, max message text 4096 chars.
- All Telegram text sent with `parse_mode=HTML` and **HTML-escaped user input** (`& < >`).
- Never block in handlers; multi-step flows (e.g. `/kick` = ban -> unban) use continuation callbacks, with context freed on both success and failure.
- Graceful shutdown on SIGINT/SIGTERM: stop accepting updates, drain in-flight requests (max 10 s), flush DB, close ring.

## 10. Security
- Token only from env/config file with mode 0600; redact in logs.
- Verify webhook secret header; reject others with 403.
- Ignore updates from private chats for admin commands.
- Validate callback_data ownership (user_id match) to prevent forged button clicks.
- Escape/parameterize all SQL (prepared statements only).
- Regex blacklist: compile with `regcomp`/PCRE2 with match limits to avoid ReDoS.

## 11. Testing & Acceptance Criteria
**Unit tests** (no network): duration parser, target resolution, HTTP parser (chunked + keep-alive), JSON mapping, token bucket, flood window, permission matrix.
**Integration tests**: mock Telegram API server (local plain-HTTP via `api_base`) replaying scripted updates and asserting outbound calls.

Acceptance:
1. 1,000 updates/sec replayed against the mock server with p99 handler latency < 50 ms and no event-loop stall > 5 ms.
2. Valgrind/ASan clean after a 10-minute soak test.
3. `/ban 10m` auto-unbans after restart of the bot process.
4. 429 responses are honored (no request sent to a throttled bucket before `retry_after`).
5. Non-admin cannot execute any admin command; bot without rights replies with a clear error.
6. Captcha timeout kicks user and cleans up the message and DB row.
7. No blocking syscalls on the main thread (verify with `strace -f -e trace=network,file` or `perf trace`).

## 12. Implementation Milestones
1. **M1**: io_uring loop, timers, eventfd, TCP connect, TLS memory-BIO handshake, one successful `getMe`.
2. **M2**: HTTP client (keep-alive, chunked), API pool, retry and rate limiter, `getUpdates` polling.
3. **M3**: Update parsing, router, DB worker, `users`/`chats` upsert, admin cache and permissions.
4. **M4**: `/ban /unban /kick /mute /unmute /del /pin /purge` + duration parser + expiry timer.
5. **M5**: Warns, settings, welcome/rules, moderation log.
6. **M6**: Automod (locks, blacklist, antiflood) and captcha.
7. **M7**: Webhook mode, graceful shutdown, tests, README with systemd unit.

## 13. Instructions to the Implementing Agent
- Implement milestone by milestone; after each, show build steps and how to test.
- Ask before deviating from any "MUST/ONLY/NEVER" rule above.
- Prefer simple, readable C; comment every io_uring state transition.
- Do not introduce additional dependencies beyond: liburing, OpenSSL, SQLite3, yyjson (or cJSON), and optionally PCRE2.
- Provide a minimal `README.md` and a sample `config.toml`.
