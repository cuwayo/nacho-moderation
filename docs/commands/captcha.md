# /captcha

Enable or disable join verification: new members must press an "I'm human"
button within a timeout or they are removed (SPEC section 5.4).

## Examples

```text
/captcha on
/captcha off
```

## Edge cases

- With the captcha enabled a new member is immediately restricted (they cannot
  send messages), sent an inline "I'm human" button and recorded in
  `pending_captcha` with a deadline (default 120 seconds).
- Pressing the button as the challenged user lifts the restriction, deletes the
  prompt and delivers the welcome message if one is configured.
- Pressing the button as any other user is rejected (the callback data carries
  the challenged user id, checked against the presser).
- When the deadline passes a periodic sweep kicks the member (ban then unban, so
  they may rejoin) and deletes the prompt.
- Both supergroup (`chat_member`) and basic-group (`new_chat_members`) joins are
  handled.
- The bot must hold `can_restrict_members`.
