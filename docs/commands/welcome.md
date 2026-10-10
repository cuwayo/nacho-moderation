# /welcome

Enable or disable the join welcome message.

## Examples

```text
/welcome on
```

Enables delivery of the saved welcome template when a member joins.

```text
/welcome off
```

Disables it without discarding the template.

## Edge cases

- The argument is exactly `on` or `off`; anything else is rejected.
- The welcome is delivered on join for both supergroups (`chat_member`) and basic
  groups (`new_chat_members`), using the template set with `/setwelcome`.
- A join by another bot never triggers a welcome.
- The bot must hold `can_change_info`.
