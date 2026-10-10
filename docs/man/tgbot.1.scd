tgbot(1)

# NAME

tgbot - asynchronous Telegram group admin and moderator bot

# SYNOPSIS

*tgbot* [*-h*] [*-V*] [*--config* _file_] [*--check-config* _file_] [*--log-level* _level_]

# DESCRIPTION

*tgbot* connects to the Telegram Bot API and moderates groups and supergroups:
it handles administrative commands, join captchas, antiflood, and blacklists.
All network I/O is asynchronous through io_uring in a single-threaded event loop.

# OPTIONS

*--config* _file_
	Path to the configuration file. Defaults to *./config.toml*.

*--check-config* _file_
	Validate the configuration file and exit. Exits 0 if valid, 1 otherwise.

*--log-level* _level_
	Override the configured log level. One of *error*, *warn*, *info*, *debug*.

*-h*, *--help*
	Print help and exit.

*-V*, *--version*
	Print the version and exit.

# COMMANDS

The available commands are listed below.

<!-- BEGIN GENERATED: commands-table -->
| Command | Role | Usage | Description |
|---|---|---|---|
| `/help` | ROLE_ANYONE |  | Show available commands |
| `/ban` | ROLE_ADMIN | `[target] [duration] [reason]` | Ban a user from the chat |
| `/unban` | ROLE_ADMIN | `[target]` | Lift a ban |
| `/kick` | ROLE_ADMIN | `[target] [reason]` | Remove a user so they can rejoin |
| `/mute` | ROLE_ADMIN | `[target] [duration] [reason]` | Restrict a user from sending messages |
| `/unmute` | ROLE_ADMIN | `[target]` | Restore a user's permissions |
| `/del` | ROLE_ADMIN |  | Delete the replied-to message and the command |
| `/pin` | ROLE_ADMIN |  | Pin the replied-to message |
| `/unpin` | ROLE_ADMIN |  | Unpin the last pinned message |
| `/purge` | ROLE_ADMIN |  | Delete messages from the replied message to now |
<!-- END GENERATED: commands-table -->

# SIGNALS

*SIGINT*, *SIGTERM*
	Stop accepting updates, drain in-flight requests (up to 10 s), flush the
	database, and exit.

*SIGUSR1*
	Dump runtime counters to the log.

# EXIT STATUS

*0*
	Success.

*1*
	Runtime or configuration error.

*2*
	Usage error (bad command-line arguments).

# FILES

*/etc/nachomoderator/config.example.toml*
	Sample configuration installed by the package.

*./config.toml*
	Default configuration path.

# ENVIRONMENT

Every configuration key can be set through its environment variable; run
*tgbot --dump-config-json* for the mapping. Environment variables are overridden
by command-line flags and override the configuration file.

# SEE ALSO

*tgbot.toml*(5)
