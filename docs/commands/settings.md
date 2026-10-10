# /settings

Open an inline-keyboard panel for the chat's settings.

## Examples

```text
/settings
```

Sends a panel with a button showing the current welcome state:

```text
Chat settings
[ Welcome: OFF ]
```

Pressing the button toggles the welcome, updates the panel in place and shows a
short confirmation.

## Edge cases

- The panel is owned by the user who opened it: a button forwarded to someone else
  is rejected ("This panel is not yours."), per SPEC section 10.
- Only administrators holding `can_change_info` may toggle settings; others get a
  refusal toast.
- Each press is acknowledged with `answerCallbackQuery` and the panel is edited
  with `editMessageText`, so the chat is not spammed with new messages.
- The panel is per chat and requires the bot to hold no specific right beyond the
  caller's `can_change_info` check.
