# SPEC ADDENDUM: Single Source of Truth (X-Macros) and Consistency Checks

> Commands and config keys are each defined ONCE, in a `.def` table. Everything else
> (enums, dispatch table, /help, setMyCommands, docs, man pages, sample config) is derived from
> or verified against that table. Supersedes the informal mentions of `commands.def` and
> `config.def` in the documentation addendum (D6, D8).

## S1. Principles

- One table per concern: `src/commands.def`, `src/config.def`. No command or key exists outside them.
- Adding a command or config key must be a one-table-row change plus handler/docs, and CI fails if any derived artifact is stale.
- Scripts never regex-parse C. The binary exports its own tables as JSON (S5), and scripts consume that. This avoids drift between the parser and the preprocessor.
- Generated content is committed only inside marked blocks in hand-written docs (S6). Never commit fully generated files other than the sample config.

## S2. `commands.def` Format

One `X(...)` per line, no multi-line entries, no preprocessor conditionals inside the file, string literals only for text fields.

```c
/* commands.def
 * X(id, name, min_role, user_right, bot_right, flags, usage, brief, handler)
 *
 *  id          UPPER_SNAKE, unique, becomes CMD_<id>
 *  name        lowercase [a-z0-9_]{1,32}, unique, Telegram command name (without '/')
 *  min_role    ROLE_ANYONE | ROLE_ADMIN | ROLE_OWNER | ROLE_SUDO
 *  user_right  RIGHT_NONE | RIGHT_RESTRICT | RIGHT_DELETE | RIGHT_PIN | RIGHT_PROMOTE | RIGHT_CHANGE_INFO
 *              (Telegram admin right the CALLER must hold; owner and sudo bypass)
 *  bot_right   same set; right the BOT must hold in the chat
 *  flags       CMDF_* OR-ed: CMDF_TARGET, CMDF_REPLY_REQUIRED, CMDF_DURATION, CMDF_REASON,
 *              CMDF_GROUP_ONLY, CMDF_HIDDEN (omit from /help and setMyCommands)
 *  usage       argument synopsis, e.g. "[target] [duration] [reason]"
 *  brief       1-256 chars, English, no trailing period requirement, shown in /help and setMyCommands
 *  handler     function name: int handler(cmd_ctx_t *ctx)
 */
X(BAN,    "ban",    ROLE_ADMIN,  RIGHT_RESTRICT, RIGHT_RESTRICT, CMDF_TARGET|CMDF_DURATION|CMDF_REASON|CMDF_GROUP_ONLY, "[target] [duration] [reason]", "Ban a user from the chat",        cmd_ban)
X(UNBAN,  "unban",  ROLE_ADMIN,  RIGHT_RESTRICT, RIGHT_RESTRICT, CMDF_TARGET|CMDF_GROUP_ONLY,                           "[target]",                    "Lift a ban",                      cmd_unban)
X(PURGE,  "purge",  ROLE_ADMIN,  RIGHT_DELETE,   RIGHT_DELETE,   CMDF_REPLY_REQUIRED|CMDF_GROUP_ONLY,                   "",                            "Delete messages from the replied message to now", cmd_purge)
X(HELP,   "help",   ROLE_ANYONE, RIGHT_NONE,     RIGHT_NONE,     0,                                                      "",                            "Show available commands",         cmd_help)
```

### Derived C artifacts (`src/commands.h` / `commands.c`)

```c
typedef enum {
#define X(id, ...) CMD_##id,
#include "commands.def"
#undef X
    CMD_COUNT
} cmd_id_t;

#define X(id, name, role, ur, br, flags, usage, brief, handler) int handler(cmd_ctx_t *ctx);
#include "commands.def"
#undef X

typedef struct {
    cmd_id_t id; const char *name; role_t min_role; right_t user_right; right_t bot_right;
    unsigned flags; const char *usage; const char *brief; int (*handler)(cmd_ctx_t *);
} cmd_desc_t;

const cmd_desc_t cmd_table[CMD_COUNT] = {
#define X(id, name, role, ur, br, flags, usage, brief, handler) \
    { CMD_##id, name, role, ur, br, flags, usage, brief, handler },
#include "commands.def"
#undef X
};
```

Compile-time/startup guarantees (each backed by a unit test, `tests/unit/test_commands_table.c`):
- Names unique and match `^[a-z0-9_]{1,32}$`; `brief` length 1..256 (Telegram limits for `setMyCommands`).
- Every handler non-NULL; `CMDF_REPLY_REQUIRED` and `CMDF_TARGET` are not both set unless documented.
- `RIGHT_*` of `user_right` is NONE when `min_role` is `ROLE_ANYONE`.
- Router lookup by name is O(1) (perfect hash or sorted table + binary search), generated from the same table.
- `/help`, `setMyCommands` payload (two scopes: `all_group_chats` = non-admin commands, `all_chat_administrators` = all non-hidden), and the man page command list are all produced from `cmd_table`. No hand-written command lists anywhere in C.

## S3. `config.def` Format

```c
/* config.def
 * K(id, toml_key, env_var, type, default, restart, secret, brief)
 *
 *  type     CFG_STR | CFG_INT | CFG_BOOL | CFG_DURATION | CFG_I64_LIST | CFG_ENUM
 *  default  C literal as string ("" for none / required); parsed with the same code as user input
 *  restart  1 if change needs restart, else 0 (hot reload on SIGHUP if 0)
 *  secret   1 => never logged, redacted in --dump-config, sample shows placeholder
 */
K(BOT_TOKEN,     "bot_token",     "BOT_TOKEN",     CFG_STR,      "",                      1, 1, "Telegram bot token from @BotFather (required)")
K(MODE,          "mode",          "MODE",          CFG_ENUM,     "polling",               1, 0, "Update source: polling or webhook")
K(WEBHOOK_LISTEN,"webhook.listen","WEBHOOK_LISTEN",CFG_STR,      "127.0.0.1:8080",        1, 0, "Webhook listen address behind a reverse proxy")
K(API_POOL_SIZE, "api.pool_size", "API_POOL_SIZE", CFG_INT,      "4",                     1, 0, "Number of keep-alive connections to the Bot API")
```

Rules:
- Precedence: CLI flag > env var > config file > default. Implemented once, driven by the table.
- Required keys are those with empty default and flagged `required` via a separate `CFG_REQUIRED(id)` list in the same file.
- Enum values come from a nested table `config_enums.def` (`E(MODE, "polling")`, `E(MODE, "webhook")`), also exported in JSON and checked against docs.
- The sample `config/config.example.toml` is **generated** from the table by `tgbot --dump-config-example` and committed; CI regenerates and diffs.
- `tgbot --check-config <file>` validates a file against the table (unknown keys are errors, with "did you mean" suggestion via edit distance <= 2).

## S4. Other Tables (same pattern, same checks)

| Table | File | Drives |
|---|---|---|
| Lock types | `locks.def` | `/lock` `/unlock` argument parsing, bitmask enum, docs list of lock types |
| API methods used | `api_methods.def` | method name strings, rate-limit bucket class (global/per-chat/bulk), idempotent flag (retry policy), mock server stub list |
| Error classes | `errors.def` | errno-style codes, log level, retry policy, user-visible message; feeds the error-handling table in `docs/architecture.md` |
| Metrics | `metrics.def` | counter names, help strings, `/metrics` and SIGUSR1 output, docs table |

`api_methods.def` MUST be consumed by the mock server test: every method in the table has a mock handler (check C9).

## S5. Introspection CLI (hidden from `--help`, documented in `docs/testing.md`)

The binary exposes read-only dump flags that need no network or token:

| Flag | Output |
|---|---|
| `tgbot --dump-commands-json` | JSON array: `{id,name,min_role,user_right,bot_right,flags[],usage,brief,hidden}` |
| `tgbot --dump-config-json` | JSON array: `{id,key,env,type,default,restart,secret,required,enum_values[],brief}` |
| `tgbot --dump-tables-json` | One object with commands, config, locks, api_methods, errors, metrics |
| `tgbot --dump-config-example` | Sample TOML to stdout |
| `tgbot --help` | Human help (golden-file tested) |

Output must be deterministic (stable ordering = table order, no timestamps), UTF-8, newline-terminated, exit code 0.

## S6. Generated Blocks in Docs

Hand-written docs contain marker pairs; the generator rewrites only the content between them:

```markdown
<!-- BEGIN GENERATED: commands-table -->
| Command | Role | Usage | Description |
|---|---|---|---|
| `/help` | ROLE_ANYONE | `` | Show available commands |
<!-- END GENERATED: commands-table -->
```

Generator: `scripts/docgen.py` (Python 3.10+, stdlib only).
- `docgen.py --update` rewrites blocks in `README.md`, `docs/commands.md`, `docs/configuration.md`, `docs/architecture.md` (error table), `docs/man/*.scd`.
- `docgen.py --check` exits non-zero if any block differs from what `--update` would produce (CI uses this).
- Blocks: `commands-table` (README compact), `commands-reference` (full per-command section skeleton with `### /name` headings, usage, rights, flags; hand-written examples live OUTSIDE the block, under the same heading), `config-table`, `config-reference`, `lock-types`, `error-classes`, `metrics-table`.
- Hand-written prose per command (examples, edge cases) lives in `docs/commands/<name>.md` snippets that the generator includes after each heading. A command without a snippet file is a check failure (C3).

## S7. Consistency Check Script

`scripts/check_consistency.py` (Python 3.10+, stdlib only; no network). Runs against a built `tgbot` binary (`--bin build/release/tgbot`) and the repo tree. Every check has a stable ID, prints `PASS|FAIL Cxx: message` with file:line when possible, and the script exits 1 if any FAIL. `--only C3,C4` and `--list` supported. Registered as CTest test `consistency` and as CI job `docs-consistency`.

| ID | Check |
|---|---|
| C1 | `docgen.py --check`: all generated blocks up to date |
| C2 | Every command in `--dump-commands-json` has a `### /<name>` heading in `docs/commands.md`, and no heading exists for a non-existent command |
| C3 | Every non-hidden command has `docs/commands/<name>.md` with sections `Examples` and `Edge cases` (non-empty) |
| C4 | Every config key has a row in `docs/configuration.md` including type, default, restart flag; no documented key missing from the table; env var names match |
| C5 | `config/config.example.toml` equals `--dump-config-example` output; it parses with `--check-config`; secrets use placeholder values |
| C6 | Every TOML snippet in `*.md` fenced as ```` ```toml ```` passes `tgbot --check-config` (unless tagged `toml no-check`) |
| C7 | Every shell snippet tagged ```` ```bash title="ci" ```` in README parses (`bash -n`) and uses only commands/presets that exist (preset names validated against `CMakePresets.json`) |
| C8 | `tgbot --help` equals `tests/golden/help.txt`; the same text appears verbatim in `docs/building.md` |
| C9 | Every method in `api_methods.def` has a handler in `tests/mock/mock_tg_server.py` and is listed in `docs/telegram-api-notes.md` |
| C10 | Every `/help` command appears in the man page `tgbot.1.scd`; every config key appears in `tgbot.toml.5.scd` |
| C11 | `setMyCommands` payload constraints: names/briefs valid; non-admin scope is a subset of admin scope |
| C12 | Doxygen groups: every `@ingroup` name is defined in `docs/groups.dox`; every source file has `@file` |
| C13 | No `TODO`/`FIXME` without issue reference `(#123)` or `(TODO-ID)` in `src/`, `tests/`, `docs/` |
| C14 | In PR context (`--base origin/main`): if `src/**` changed, `CHANGELOG.md` `Unreleased` changed (unless label/flag `--skip-changelog`) |
| C15 | If `commands.def` or `config.def` changed in a PR, at least one file under `tests/` and one under `docs/` changed too |
| C16 | Metrics in `metrics.def` all documented; error classes in `errors.def` all appear in architecture error table |
| C17 | Relative links and anchors inside `docs/` and README resolve (fast built-in check; `lychee` covers the rest) |
| C18 | Terminology: forbidden/ambiguous terms list in `docs/terminology.txt` (e.g. "blacklist" vs "denylist" decision, "supergroup" capitalization) enforced by regex in docs |

### Script quality requirements
- Script has its own tests: `tests/scripts/test_check_consistency.py` (unittest) using fixture mini-repos with deliberately broken docs; each check C1..C18 has a passing and failing fixture.
- Output formats: human (default), `--format github` (emits `::error file=...,line=...::msg` annotations), `--format json`.
- Runs in < 10 s. No dependency outside Python stdlib and the built binary.
- Fails with a clear message and exit code 2 (not 1) on tool errors (binary missing, invalid JSON) so CI can distinguish "inconsistent" from "broken".

## S8. Developer Workflow ("add a command" checklist, goes into CONTRIBUTING.md)

1. Add one `X(...)` row to `src/commands.def`.
2. Create `src/cmd_<group>.c` with `int cmd_<name>(cmd_ctx_t *ctx)` and full Doxygen comments.
3. Add unit tests (permission matrix row, argument parsing) and an integration scenario in `tests/mock/scenarios/`.
4. Create `docs/commands/<name>.md` (Examples, Edge cases).
5. Run `cmake --build --preset debug --target docs-update` (runs `docgen.py --update`).
6. Run `ctest --preset asan` and `scripts/check_consistency.py --bin build/debug/tgbot`.
7. Add a CHANGELOG `Unreleased` entry.

Equivalent checklist for config keys (`config.def`, validator tests, `docs-update`, regenerate sample config) and for metrics.

CMake targets to add: `docs-update` (docgen --update), `docs-verify` (docgen --check + check_consistency), `gen-config-example`.

## S9. Acceptance Criteria

1. Deleting any one of: a command heading in `docs/commands.md`, a config key row, a snippet file, a man page entry, or a mock handler makes `check_consistency.py` fail with the matching check ID.
2. Adding a new `X(...)` row with no other change fails C1/C2/C3 (and compiles only if the handler exists).
3. Running `docgen.py --update` twice is idempotent (second run changes nothing).
4. `tgbot --dump-tables-json | jq` output is byte-identical across two runs and across GCC/Clang builds.
5. `/help` output in a real chat, the `setMyCommands` payload, README command table, and the man page list the same commands in the same order.
6. All check scripts pass on a fresh clone in the CI container using only the documented prerequisites.

## S10. Instructions to the Implementing Agent (additions)

- Implement S2/S3 tables, the dump flags (S5), `docgen.py`, and `check_consistency.py` with C1 to C5 in milestone **M0**, even though the table has only `help` and a couple of keys at first. Add checks C6 to C18 as the related features land.
- Never hand-edit between `BEGIN GENERATED` / `END GENERATED` markers; change the table or the generator.
- Do not add a command, config key, lock type, API method, error class, or metric anywhere except its `.def` file.
- When a check fails, fix the cause. Do not weaken, skip, or delete a check without an ADR and my approval.
- Keep `.def` files regular (one entry per line) so they stay greppable and diff-friendly.
