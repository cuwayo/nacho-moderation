# /setrules

Set the chat rules text.

## Examples

```text
/setrules Be kind. No spam. English or Indonesian only.
```

Saves the rules; members can read them with `/rules`.

## Edge cases

- The rules are free text and are shown verbatim by `/rules`.
- Saving overwrites any previous rules.
- The rules are per chat and persisted in the `chats` table.
- The bot must hold `can_change_info`.
