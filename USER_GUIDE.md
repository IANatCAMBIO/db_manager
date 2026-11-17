# camb.io Database Manager — user guide

Everything after [README.md](README.md) has you installed. Building it is
in [BUILD.md](BUILD.md).

- [The coin and its menu](#the-coin-and-its-menu)
- [The window](#the-window)
- [Connections](#connections)
- [The integrity check](#the-integrity-check)
- [Backups](#backups)
- [Where things are](#where-things-are)
- [Starting at login](#starting-at-login)
- [Running from the source tree](#running-from-the-source-tree)

## The coin and its menu

The coin in the menu bar is the whole of the app's presence. A **left
click** opens the window under it, or closes it when it is up. A
**right click** (or ⌃-click) opens the menu:

| Item | Does |
|---|---|
| Show Database Window | the window, under the coin |
| Check Database Now | the integrity check, now (as **Update** in the window) |
| Back Up Now | one backup pass, whatever the schedule |
| About camb.io Database Manager | the About panel: version and build date |
| Quit camb.io Database Manager | stops the daemon: no backups or checks until it runs again |

Hovering the coin shows the latest event: the last backup made, the last
check's verdict, or why the last pass failed.

## The window

One column, the Database section the Notes and Tasks Settings windows
used to have, moved here.

**The plate** states facts about the file and never changes it:

| Line | Meaning |
|---|---|
| Health | the light and the verdict of the last integrity check, and when it was made: green for one that ran and passed, red for one that found problems or could not run, grey before any check. The tooltip has the findings. |
| Current database | the file's path; **copy** puts it on the clipboard |
| Data | how many notes (not in the Trash), tasks (not deleted) and tags it holds |
| Size on disk | the file's size, free pages included |
| SHA-256 | the file's fingerprint as it stands, shortened; the tooltip and **copy** have the whole of it. A database in use changes with the next edit, so this moves. |

**Update** runs the integrity check and re-reads every line.

**Back up the database automatically** is the master switch for the
schedule. **Every _n_ minutes, keeping _m_ files** sets the interval (0
backs up only on Back Up Now) and the retention (at least 1). The line
under them names the folder the copies go to, warns when that folder is
inside the database's own, says when the last backup was made, and
repeats why the last pass failed if it did. **Change Folder…** picks the
destination; **Back Up Now** makes one pass regardless of the schedule.

**Connections** says who has the database open and what a crash left
behind (see [Connections](#connections)).

**Start camb.io Database Manager at login** registers the app as a login item
(see [Starting at login](#starting-at-login)).

The line at the bottom shows the latest event.

Every control writes through at once: the policy goes into the database's
own `shared_settings` table, so it holds wherever that file is opened.

## Connections

**Open in …** lists every process of yours that has the database file,
or its write-ahead log, open right now: Notes, Tasks, this app, an
agent's `python3` or `sqlite3`, each with its process id. It is found by
looking through the open files of your processes (libproc), refreshed
every few seconds while the window is up. Other users' processes cannot
be seen.

**There are no orphaned connections.** An SQLite connection is nothing
but open files and advisory locks inside one process; when that process
crashes, macOS closes the files and releases the locks, and the next
opener recovers the file from its journal. So the list above is the
whole truth about who is connected. What a crash or an interruption
*can* leave behind, the second line reports:

| Left behind | Meaning | What happens |
|---|---|---|
| write-ahead log content | edits committed but not yet folded into the main file (`shared_data.db-wal`) | the next writer folds them in; nothing to do |
| a backup claim | a backup pass claimed its turn (`backup_claim_at`) and never wrote `backup_last_at` | ignored once ten minutes old; a live one may belong to a pass in another process |
| an unfinished backup folder | `.in-progress-…` in the backup folder, from a pass that died mid-copy | swept by the next pass once it is an hour old |

When there is nothing, the line says so.

## The integrity check

The check is the shared module's (`shared_db_check`): `PRAGMA
integrity_check`, `PRAGMA foreign_key_check`, the two stamps that mark
the file as a shared database, WAL mode, and the cross-table rules Notes
and Tasks rely on (every action-item link names a note and a task, the
uid counter is ahead of every uid, no subtask of a subtask, every tag is
carried by something, both change counters exist).

It runs at launch, once a day while the app is running, and on
**Update** or **Check Database Now**. It runs on a read-only connection
of its own and writes nothing: a verdict stamped into the file would make
the check change the file it measured, and the fingerprint beside it
move. So the verdict lives in memory for the life of the process and
does not survive a restart — the plate says "Not checked" until the
launch check lands.

A check that finds problems, or cannot run, is shown in a panel at once,
whoever asked for it. One that passes is a line in the status bar and
the coin's tooltip.

## Backups

Backups are on by default: every hour, keeping the newest 48 (two days).
Every minute the daemon asks the database whether one is due under the
policy stored in it; the pass itself is the shared module's:

1. a `VACUUM INTO` copy is written inside a private `.in-progress-…`
   directory in the backup folder;
2. it is verified (`integrity_check` and both stamps on a read-only
   connection);
3. only then is it linked to its final name,
   `shared_data-YYYYMMDD-HHMMSSZ.db` — the time in **UTC**, so names sort
   across daylight-saving changes; `_01`…`_99` before `.db` when that
   second is taken;
4. only then are the oldest backups beyond the retention removed — and
   only files named like a backup.

A crash leaves at most an `.in-progress-…` directory, which the next pass
sweeps once it is an hour old. The database and its `-wal` / `-shm`
files are never deleted, renamed or truncated, and never `VACUUM`ed in
place.

**Where they go.** The folder chosen with Change Folder…, else `backups/`
in the directory of the database file (for the default file,
`~/Library/Application Support/camb.io/backups/`). The window warns when
the folder is beside the database: those copies are real backups, but a
disk that takes the database takes them too. Point them at a disk the
database does not live on.

**What the database records.** `backup_last_at` (the last verified copy),
`backup_last_error` (why the last pass failed — also set when a copy was
made but old ones could not be removed), and a ten-minute claim
(`backup_claim_at`) so that two processes polling at once make one
backup. The policy keys are `backup_enabled`, `backup_interval_min`,
`backup_keep` and `backup_dir`.

**Restoring.** Quit camb.io Database Manager, Notes and Tasks; move
`shared_data.db` (and its `-wal` and `-shm` files, if present) aside;
copy the chosen backup to `shared_data.db`; start camb.io Database Manager. The
`shared-data` skill in the `shared_data` repository has the same recipe
for an agent.

## Where things are

| What | Where |
|---|---|
| The shared database (default) | `~/Library/Application Support/camb.io/shared_data.db` |
| Its journal and shared memory | `shared_data.db-wal`, `shared_data.db-shm` beside it |
| Backups (default) | `~/Library/Application Support/camb.io/backups/` |
| The one-copy lock | `camb.io_db_mgr.lock` beside the database |
| A database kept elsewhere | the path in `~/Library/Application Support/camb.io/location` (one line), if that file exists |
| The app | `~/Applications/camb.io_db_mgr.app` |

The location rule is the shared module's: `$CAMBIO_SHARED_DB` when set
(an absolute path), else the `location` pointer file, else the default.
camb.io Database Manager, Notes and Tasks all follow it, so they always open the
same file. The database must be on a local disk, outside iCloud Drive,
Dropbox and other synced folders; the module refuses such a file, and
the app then quits with the reason.

There is no settings file of the app's own: the backup policy is in the
database, the login item is macOS's.

## Starting at login

The daemon makes backups and runs checks only while it is running, so
the window's **Launch** section registers it as a login item through
`SMAppService`. macOS ties the registration to the bundle that made it:
register from the installed copy in `~/Applications`, not from a `make
run` in the source tree. The first registration may need your approval
under System Settings ▸ General ▸ Login Items; the checkbox shows the
real state after the attempt. Turning it off there, or in the window,
unregisters it.

## Running from the source tree

`make run` runs the daemon on a sandbox database at
`dev/data/shared_data.db` (created on the first run, as the app would
create the real one), never the per-user file: the Makefile sets
`CAMBIO_SHARED_DB` for every run and test. Backups of the sandbox go to
`dev/data/backups/`. Quit it from the coin's menu or with Ctrl-C.
