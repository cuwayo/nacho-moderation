-- Schema v1 for nacho-moderator (mirrors the original SPEC section 6).
-- Applied idempotently by the migration runner; the schema_version table
-- records which migration has been applied.

CREATE TABLE IF NOT EXISTS chats (
    chat_id          INTEGER PRIMARY KEY,
    title            TEXT,
    type             TEXT,
    welcome_enabled  INTEGER DEFAULT 0,
    welcome_text     TEXT,
    rules_text       TEXT,
    captcha_enabled  INTEGER DEFAULT 0,
    captcha_timeout  INTEGER DEFAULT 120,
    warn_limit       INTEGER DEFAULT 3,
    warn_action      TEXT DEFAULT 'mute',
    flood_limit      INTEGER DEFAULT 0,
    flood_window     INTEGER DEFAULT 10,
    locks            INTEGER DEFAULT 0,
    log_chat_id      INTEGER,
    lang             TEXT DEFAULT 'en'
);

CREATE TABLE IF NOT EXISTS users (
    user_id    INTEGER PRIMARY KEY,
    username   TEXT COLLATE NOCASE,
    first_name TEXT,
    last_seen  INTEGER
);
CREATE INDEX IF NOT EXISTS idx_users_username ON users(username);

CREATE TABLE IF NOT EXISTS warns (
    id         INTEGER PRIMARY KEY,
    chat_id    INTEGER,
    user_id    INTEGER,
    admin_id   INTEGER,
    reason     TEXT,
    created_at INTEGER
);
CREATE INDEX IF NOT EXISTS idx_warns ON warns(chat_id, user_id);

CREATE TABLE IF NOT EXISTS actions (
    id         INTEGER PRIMARY KEY,
    chat_id    INTEGER,
    user_id    INTEGER,
    admin_id   INTEGER,
    action     TEXT,
    reason     TEXT,
    created_at INTEGER,
    expires_at INTEGER,
    lifted     INTEGER DEFAULT 0
);
CREATE INDEX IF NOT EXISTS idx_actions_exp
    ON actions(expires_at) WHERE lifted = 0 AND expires_at IS NOT NULL;

CREATE TABLE IF NOT EXISTS blacklist (
    chat_id  INTEGER,
    pattern  TEXT,
    is_regex INTEGER DEFAULT 0,
    PRIMARY KEY (chat_id, pattern)
);

CREATE TABLE IF NOT EXISTS pending_captcha (
    chat_id    INTEGER,
    user_id    INTEGER,
    message_id INTEGER,
    nonce      TEXT,
    deadline   INTEGER,
    PRIMARY KEY (chat_id, user_id)
);
