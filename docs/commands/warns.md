# /warns

List warnings: your own, or (for administrators) another user's.

## Examples

```text
/warns
```

Lists your own warnings, so any member can check their standing:

```text
You have 2 warning(s):
1. repeated advertising
2. spamming links
```

```text
/warns @spammer
```

Lists another user's warnings; this form is admin-only.

## Edge cases

- With no target the command is self-service: any member sees their own warnings.
- Naming another user (a reply, `@username` or numeric id) requires an
  administrator, owner or sudo caller; others get a refusal.
- At most one page (16 warnings) is listed; `/resetwarns` clears them all.
- A warning without a reason is shown as `(no reason)`.
- Read-only: listing does not require any Telegram right.
