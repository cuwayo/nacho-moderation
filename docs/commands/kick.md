# /kick

Remove a user from the chat but let them rejoin. Implemented as a ban followed
immediately by an unban in the ban's completion callback (SPEC section 5.2).

## Examples

```text
/kick @noise
```

Kicks the user and replies `kicked`.

## Edge cases

- The two API calls are chained: the unban is only issued after the ban succeeds.
  If the ban fails, the bot reports the error and does not unban.
- The invocation is reference-counted, so the unban callback safely runs after the
  handler has returned (SPEC section 9).
- The owner, other administrators and the bot itself cannot be kicked.
- `/kick` takes no duration; it never leaves the user banned.
