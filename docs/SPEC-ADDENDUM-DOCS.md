# SPEC ADDENDUM: Documentation (README, Doxygen, Project Docs)

> Documentation is a deliverable, not an afterthought. Every milestone's Definition of Done
> (A9 in the toolchain addendum) includes the documentation checks in D9.

## D1. Documentation Toolchain

| Purpose | Tool | Notes |
|---|---|---|
| API reference from source | **Doxygen >= 1.9.8** | Config in `docs/Doxyfile.in`, processed by CMake `configure_file`. |
| Diagrams in Doxygen | Graphviz (`dot`) | Call graphs, include graphs, collaboration diagrams. |
| Diagrams in prose docs | Mermaid (rendered natively by GitHub) | Used in `README.md` and `docs/*.md` only. Doxygen does not render Mermaid, so for diagrams that must appear in the Doxygen site use `@dot` or `@startuml` (PlantUML, optional). |
| Markdown lint | `markdownlint-cli2` | Config `.markdownlint.yaml` committed. |
| Link checker | `lychee` | Fails CI on dead internal links; external links are warnings only. |
| Spell check | `codespell` (+ optional `cspell` with a project dictionary) | Same hook as code. |
| Man page | Hand-written `scdoc` or `pandoc` from Markdown | Output `tgbot.1` and `tgbot.toml.5`. |
| Changelog | Keep a Changelog format, generated/curated from Conventional Commits | `git-cliff` allowed. |
| Hosting | GitHub Pages (Doxygen HTML), published from CI on `main` and on tags | |
| Optional theme | `doxygen-awesome-css` pinned as a submodule | Purely cosmetic, must not be required to build docs. |

CMake targets: `docs` (build Doxygen HTML to `build/docs/html`), `docs-check` (Doxygen with `WARN_AS_ERROR=FAIL_ON_WARNINGS` plus markdownlint plus link check), `man` (build man pages).
`docs` must be optional: if Doxygen is missing, configure succeeds with a status message and the target is skipped.

## D2. Documentation Layout

```
/README.md                     # entry point, doubles as Doxygen main page
/CHANGELOG.md
/CONTRIBUTING.md
/SECURITY.md
/LICENSE
/CODE_OF_CONDUCT.md            # optional
/docs/
  Doxyfile.in
  DoxygenLayout.xml            # optional, customizes navigation
  index.md                     # docs landing page (links to everything below)
  architecture.md              # event loop, ops, threads, data flow
  building.md                  # prerequisites, presets, sanitizers, troubleshooting
  configuration.md             # every config key, defaults, examples
  commands.md                  # every bot command: syntax, rights, examples
  deployment.md                # systemd, Docker, reverse proxy for webhook, seccomp note
  testing.md                   # how to run unit/integration/fuzz/soak/coverage
  io_uring-notes.md            # kernel requirements, feature probing, pitfalls
  telegram-api-notes.md        # rate limits, retry rules, endpoints used
  troubleshooting.md           # FAQ, common errors, how to read logs
  adr/
    0000-template.md
    0001-single-thread-io_uring-loop.md
    0002-tls-via-openssl-memory-bio.md
    0003-sqlite-in-worker-thread.md
    ...
  diagrams/                    # .dot / .puml sources (never commit only the rendered PNG)
  man/
    tgbot.1.scd
    tgbot.toml.5.scd
```

## D3. README.md Specification

README must be readable in under 5 minutes and let a new user go from clone to running bot. Required sections, in this order:

1. **Title + one-line description**, then badges (CI status, coverage, license, latest release, docs link).
2. **Features**: short bullet list (moderation commands, captcha, antiflood, io_uring async, etc.).
3. **How it works** (high level): one Mermaid architecture diagram and 3-5 sentences.
4. **Requirements**: OS, minimum kernel (>= 5.15, recommended 6.1+), compilers, libraries with versions, with `apt`/`dnf` install one-liners.
5. **Quick start**: copy-pasteable commands that work on a clean Ubuntu 24.04:
   - clone, configure (`cmake --preset release`), build, run tests
   - create a bot with @BotFather, obtain token
   - write `config.toml` (minimal example)
   - run the bot, add it to a group, grant admin rights, verify with `/help`
6. **Configuration**: summary table of the most important keys, link to `docs/configuration.md` for the full list.
7. **Commands**: compact table (command, purpose, required right), link to `docs/commands.md`.
8. **Deployment**: systemd quick install, Docker note (io_uring and seccomp), link to `docs/deployment.md`.
9. **Development**: build presets, running tests/sanitizers/fuzzers/coverage, generating docs (`cmake --build --preset debug --target docs`), link to CONTRIBUTING.
10. **Project layout**: short tree with one-line descriptions.
11. **Troubleshooting**: top 5 issues (io_uring blocked by seccomp/sysctl `kernel.io_uring_disabled`, TLS cert errors, missing admin rights, 429 flood, DB locked).
12. **Security**: pointer to SECURITY.md and note about token handling.
13. **License** and **Acknowledgements**.

README rules:
- Every command in the README must be tested in CI (a `docs-smoke` job executes the quick-start build/test steps verbatim extracted from fenced blocks tagged ```` ```bash title="ci" ````, or an equivalent script kept in sync).
- No screenshots as the only source of information; include text alternatives.
- Use relative links for repo files; no absolute `github.com/<user>/...` links to the same repo.
- No secrets or real chat/user IDs in examples; use obviously fake values (`123456:ABC-DEF...`, `-1001234567890`).
- Max line width not enforced in prose; one sentence per line is allowed for diff-friendliness.

## D4. Doxygen Configuration (`Doxyfile.in`)

Required settings:

```
PROJECT_NAME           = "tgbot"
PROJECT_NUMBER         = @PROJECT_VERSION@
PROJECT_BRIEF          = "Asynchronous Telegram group admin bot in C using io_uring"
OUTPUT_DIRECTORY       = @CMAKE_BINARY_DIR@/docs
INPUT                  = @CMAKE_SOURCE_DIR@/README.md @CMAKE_SOURCE_DIR@/src @CMAKE_SOURCE_DIR@/include @CMAKE_SOURCE_DIR@/docs
FILE_PATTERNS          = *.c *.h *.md
RECURSIVE              = YES
EXCLUDE                = @CMAKE_SOURCE_DIR@/third_party @CMAKE_SOURCE_DIR@/tests/fixtures
USE_MDFILE_AS_MAINPAGE = @CMAKE_SOURCE_DIR@/README.md
MARKDOWN_SUPPORT       = YES
OPTIMIZE_OUTPUT_FOR_C  = YES
EXTRACT_ALL            = NO          # undocumented symbols must warn
EXTRACT_STATIC         = YES
EXTRACT_PRIVATE        = NO
JAVADOC_AUTOBRIEF      = YES
QT_AUTOBRIEF           = NO
BUILTIN_STL_SUPPORT    = NO
SOURCE_BROWSER         = YES
REFERENCED_BY_RELATION = YES
REFERENCES_RELATION    = YES
GENERATE_HTML          = YES
GENERATE_LATEX         = NO
GENERATE_TREEVIEW      = YES
HAVE_DOT               = YES
CALL_GRAPH             = YES
CALLER_GRAPH           = YES
INCLUDE_GRAPH          = YES
INCLUDED_BY_GRAPH      = YES
DOT_IMAGE_FORMAT       = svg
DOT_TRANSPARENT        = YES
INTERACTIVE_SVG        = YES
WARN_IF_UNDOCUMENTED   = YES
WARN_IF_DOC_ERROR      = YES
WARN_IF_INCOMPLETE_DOC = YES
WARN_NO_PARAMDOC       = YES
WARN_AS_ERROR          = FAIL_ON_WARNINGS   # in docs-check/CI; NO for local `docs`
WARN_LOGFILE           = @CMAKE_BINARY_DIR@/docs/doxygen-warnings.log
ALIASES                = "ownership=\par Ownership:\n" \
                         "thread_safety=\par Thread safety:\n" \
                         "io_state=\par io_uring state:\n" \
                         "errors=\par Errors:\n"
PREDEFINED             = DOXYGEN_SHOULD_SKIP_THIS
```

Notes:
- Doxygen must treat `README.md` as the main page, so README links to other docs must work both on GitHub (relative path) and in Doxygen (use `@ref`/`\subpage` only in `docs/index.md`, not in README).
- Mermaid blocks in `README.md` are wrapped so Doxygen ignores them, or placed only in files excluded from Doxygen (`docs/architecture.md` carries a `@dot` equivalent for the HTML site).

## D5. Source Comment Conventions (Doxygen)

Style: **Javadoc-style** `/** ... */` blocks with `@` commands. One style across the project.

### File header (every `.c` and `.h`)
```c
/**
 * @file tg_api.h
 * @brief Telegram Bot API client: connection pool, rate limiting, retries.
 *
 * Longer description: responsibilities, what this module does NOT do,
 * and how it fits in the event loop.
 *
 * @ingroup api
 */
```

### Module groups
Define groups in a single `docs/groups.dox`: `loop`, `net`, `tls`, `http`, `api`, `router`, `commands`, `automod`, `perms`, `db`, `cache`, `util`. Every public symbol belongs to exactly one group via `@ingroup`.

### Functions (all non-static functions in headers are mandatory; static functions need at least `@brief`)
```c
/**
 * @brief Queue a ban for a chat member.
 *
 * Sends `banChatMember` through the rate limiter. Never blocks; the callback
 * runs on the event-loop thread.
 *
 * @param[in]  api       API client instance (must outlive the request).
 * @param[in]  chat_id   Target chat (negative for groups/supergroups).
 * @param[in]  user_id   User to ban.
 * @param[in]  until     Unix time when the ban expires; 0 = permanent.
 * @param[in]  cb        Completion callback, never NULL.
 * @param[in]  ctx       Opaque pointer passed back to @p cb.
 *
 * @return 0 if queued, negative errno on failure (-ENOMEM, -EINVAL).
 *         The callback is NOT invoked when this function returns an error.
 *
 * @ownership @p ctx is owned by the caller and must remain valid until @p cb runs.
 * @thread_safety Event-loop thread only.
 * @errors Network errors, HTTP 4xx/5xx and Telegram error codes are reported
 *         through @p cb, not the return value.
 *
 * @see tg_api_unban_member
 */
int tg_api_ban_member(tg_api_t *api, int64_t chat_id, int64_t user_id,
                      time_t until, tg_api_cb cb, void *ctx);
```

Required tags per function: `@brief`, every `@param` with direction `[in]/[out]/[in,out]`, `@return` (or none for `void`), `@ownership` when pointers/memory cross the boundary, `@thread_safety`. Add `@errors`, `@pre`, `@post`, `@note`, `@warning`, `@see` where relevant.

### Types, structs, enums, macros
- Every struct and every field documented (`/**< ... */` trailing comments for fields, including units: ms vs s, bytes, etc.).
- Every enum value documented.
- Every public macro documented with `@def` semantics and usage example when non-obvious.
- State machines (connection, request, captcha) documented with an `@dot` state diagram in the header of the owning module, and every transition in code carries the `/* STATE: X -> Y on event Z */` comment (from the code standards). Use `@io_state` alias to list which io_uring ops are in flight per state.

### Implementation comments
- Explain **why**, not what. No comments that restate the code.
- Non-obvious invariants: `/* INVARIANT: ... */`. Known limitations: `/* TODO(name): ... */` or `/* FIXME ... */` with an issue number; CI greps for TODO without issue reference.
- Safety-critical sections (buffer bounds, TLS BIO pumping, SQE lifetime) get a comment block explaining the reasoning.

### Documentation of tests
- Each test file starts with a `@file`/`@brief` stating which module and behaviors it covers.
- Each test function name states behavior: `test_duration_parse_rejects_overflow`. Brief `/** @brief */` only when the name is insufficient.
- Tests are excluded from the Doxygen reference by default (`EXCLUDE_PATTERNS = */tests/*`) but the test strategy is documented in `docs/testing.md`.

## D6. Prose Documentation Requirements

### docs/architecture.md
- Component diagram (the one from the main spec, as Mermaid for GitHub and `@dot` for Doxygen).
- Sequence diagrams for: update received -> router -> command -> API call -> response -> DB write; captcha flow; rate-limited request with 429; graceful shutdown.
- Thread model: which thread owns what, how `eventfd` hand-off works, which data is shared and how it is protected.
- Buffer/memory model: per-update arena, buffer ring, lifetimes of `op_t` contexts.
- Error-handling policy table (error class -> retry? -> log level -> user-visible?).

### docs/commands.md
Auto-generated or hand-written from a single source of truth (`commands.def` X-macro table in code). **A test must fail if a registered command is missing from the docs** (CI script compares the command table with headings in `commands.md`).
Per command: syntax, arguments, duration format, required user right, required bot right, behavior, reply examples, edge cases (e.g. cannot ban admins), related settings.

### docs/configuration.md
Every key: name, type, default, example, effect, whether restart is required. Generated check: a test parses `config.def` (X-macro table) and verifies each key appears in this file.

### docs/adr/ (Architecture Decision Records)
MADR-style template: Title, Status (proposed/accepted/superseded), Context, Decision, Consequences, Alternatives considered. Any change to a decision tagged MUST/ONLY/NEVER in the main spec requires a new ADR.

### CONTRIBUTING.md
Dev environment setup, branch/PR flow, Conventional Commits, Definition of Done checklist, how to add a command (step-by-step: add to `commands.def`, handler file, tests, docs, changelog), how to add a config key, how to add a fuzz target, code style (links to `.clang-format`/`.clang-tidy`).

### SECURITY.md
Supported versions, private vulnerability reporting channel, scope (token leakage, privilege escalation via commands, parser bugs), expected response times, hardening notes.

### CHANGELOG.md
Keep a Changelog format, SemVer. Sections: Added, Changed, Deprecated, Removed, Fixed, Security. `Unreleased` section always present; every user-visible PR updates it.

### Man pages
`tgbot(1)`: synopsis, options (`--config`, `--check-config`, `--version`, `--log-level`), signals (SIGINT/SIGTERM/SIGUSR1 behavior), exit codes, files, environment, examples.
`tgbot.toml(5)`: configuration file reference (same content source as `configuration.md`).
Installed by CMake `install()` to `share/man`.

### CLI help
`tgbot --help` output is documented verbatim in README or `docs/building.md` and checked by a test (golden file) so docs and binary cannot drift.

## D7. Writing Style Guide

- Audience: a C developer comfortable with Linux but new to io_uring and Telegram bot APIs; and a sysadmin who only deploys the bot (separate sections).
- Language: English, present tense, active voice, second person for instructions ("Run...").
- Short sentences; one idea per paragraph; define acronyms at first use (TLS, SQE, CQE, MPSC).
- Code blocks always have a language tag (` ```c `, ` ```bash `, ` ```toml `, ` ```sql `).
- Shell examples: prefix with `$` only if output is shown; otherwise paste-ready without prompt.
- Admonitions via blockquote labels: `> **Note:**`, `> **Warning:**`.
- Terminology table kept in `docs/index.md` (e.g. "chat" vs "group" vs "supergroup", "restrict" vs "mute"); use consistently.
- Version-sensitive claims (kernel features) state the minimum version explicitly.
- Every example config/command in docs must be verified by a CI test or script (no untested examples).

## D8. CI Integration (additions to ci.yml)

| Job | Steps |
|---|---|
| `docs-lint` | markdownlint-cli2, codespell on `*.md`, lychee on internal links |
| `docs-build` | install doxygen + graphviz, `cmake --build --preset debug --target docs-check`, upload `docs/html` artifact. Fail on any Doxygen warning. |
| `docs-consistency` | script checks: (a) every command in `commands.def` appears in `docs/commands.md`; (b) every key in `config.def` appears in `docs/configuration.md`; (c) `tgbot --help` matches golden file; (d) no `TODO` without issue id; (e) CHANGELOG `Unreleased` modified when `src/` changes (PR label `skip-changelog` bypasses) |
| `docs-smoke` | execute README quick-start build/test commands in a clean container |
| `docs-deploy` | on `main`/tag: publish Doxygen HTML to GitHub Pages (`gh-pages` branch or Pages artifact), versioned folder per tag plus `latest` |

## D9. Documentation Definition of Done (add to every PR / milestone)

- [ ] Every new/changed public function, struct, enum, macro has complete Doxygen comments (`docs-check` passes with zero warnings)
- [ ] New module has `@file` header and `@ingroup` assignment
- [ ] State machines changed -> diagram and `STATE:` comments updated
- [ ] New/changed command -> `commands.def`, `docs/commands.md`, `/help` text, and tests updated
- [ ] New/changed config key -> `config.def`, `docs/configuration.md`, sample `config.toml` updated
- [ ] Architectural decision changed -> new or superseding ADR added
- [ ] README quick-start still works (docs-smoke green)
- [ ] CHANGELOG `Unreleased` updated
- [ ] No dead links, markdownlint and spell check clean

## D10. Documentation Coverage Targets

- Doxygen: 100% of non-static functions, structs, enums, and macros in headers documented (enforced by `WARN_IF_UNDOCUMENTED` + `WARN_AS_ERROR`).
- `@param` and `@return` completeness enforced via `WARN_NO_PARAMDOC`.
- Optional metric: `doxygen-coverage` or `coverxygen` report uploaded as CI artifact; threshold 95% including static functions' `@brief`.

## D11. Instructions to the Implementing Agent (additions)

- Create the documentation skeleton in milestone **M0**: Doxyfile.in, `docs` and `docs-check` targets, README with placeholder sections, CONTRIBUTING, CHANGELOG, markdownlint config, docs CI jobs.
- Write Doxygen comments in the same commit as the code; do not leave them for later. A function without documentation is an incomplete function.
- Do not invent behavior in docs: document what the code does; when code and spec disagree, stop and ask.
- Keep a single source of truth (X-macro tables) for commands and config keys and generate or verify docs from them.
- At the end of each milestone, output: updated README sections, the list of docs files changed, the output of `docs-check`, and a screenshot-free description of how to browse the generated HTML.
- Never commit generated HTML, only sources (`.dox`, `.md`, `.dot`, `.puml`, `Doxyfile.in`).
