# /setwelcome

Set the message sent when a new member joins (SPEC section 5.4).

## Examples

```text
/setwelcome Welcome {first} to {chat}! You are member #{count}.
```

Saves the template; enable delivery with `/welcome on`.

## Edge cases

- Placeholders: `{first}` (first name), `{mention}` (a mention link),
  `{chat}` (chat title) and `{count}` (member count). An unknown `{...}` token is
  left as-is.
- Substituted names are HTML-escaped, so a name cannot inject markup; the template
  itself may contain Telegram HTML.
- Saving a template does not enable delivery; use `/welcome on`.
- The template is per chat and persisted in the `chats` table.
- The bot must hold `can_change_info`.
