# Building camb.io Database Manager

## Toolchain and dependencies

| What | Version used | How to get it |
|---|---|---|
| clang (Apple) | Xcode command-line tools | `xcode-select --install` |
| SQLite | 3.53 (MacPorts; ≥ 3.27 for `VACUUM INTO`) | `sudo port install sqlite3` |
| pkg-config | MacPorts `pkgconf` | `sudo port install pkgconf` |
| `shared_data` (the shared database module: schema, stamps, location, connections, backups, check) | the repository beside this one | `git clone … shared_data` next to `cambio_db` |
| `shared_ui` (the tool panel, the copy link, the About panel, formatting) | the repository beside this one | `git clone … shared_ui` next to `cambio_db` |

The Makefile finds `pkg-config` on `PATH` or at `/opt/local/bin/pkg-config`
and takes SQLite's flags from it. **MacPorts SQLite on purpose:** Notes,
Tasks and this app open one database file at once (WAL), so they link one
SQLite rather than three versions. No GLib.

Frameworks: Cocoa, ServiceManagement (the login item),
UniformTypeIdentifiers (shared_ui's file picker), CoreFoundation, and, in
libSystem, CommonCrypto for SHA-256 and libproc for the scan of which
processes hold the database open.

**The shared modules.** `SHARED_DATA_DIR ?= $(CURDIR)/../shared_data`:
`src/shared_db.c` is compiled into the core with the core's flags.
`SHARED_UI_DIR ?= $(CURDIR)/../shared_ui`: `shared_ui.m` and
`shared_tool_panel.m` are compiled in with the app's Objective-C flags
(the day planner, calendar, hotkey and the rest of shared_ui are not
needed and not built). Change either in its own repository, for every
app.

## Targets

| Target | Does |
|---|---|
| `make` | builds `camb.io_db_mgr.app` in the repo root; incremental, header dependencies via `-MMD -MP`; the icon set from `icons/coin.png` with `sips` + `iconutil` |
| `make run` | builds, then runs the daemon in the foreground on `dev/data/shared_data.db` (`CAMBIO_SHARED_DB`), log on the terminal |
| `make install` | builds; stops a running installed copy (SIGTERM, an ordinary Quit; SIGKILL after 5 s), copies the bundle to `~/Applications`, reopens it if it was running |
| `make clean` | removes `build/` and the bundle |
| `make clean-dev` | removes `dev/` (the sandbox and the test data) |
| `make test` | the four release checks below; not a dependency of anything else |
| `make test-build` | a from-scratch build with zero warnings; `plutil -lint`, executable present, `codesign --verify` |
| `make test-unit` | `tests/unit/*_test.c`, linked against the core alone |
| `make test-features` | `tests/features/*_test.c`, the daemon's passes on a scratch database |
| `make test-ui` | `tests/ui/ui_check.m`: the window built in-process, its chrome asserted, light and dark PNG snapshots in `dev/snapshots/` |

Flags: `-O2 -Wall -Wextra -Werror=unguarded-availability`, compiled for
macOS 14 (`-mmacosx-version-min=14.0`), linked for the build machine's
macOS because MacPorts builds its SQLite for that system (the linker
warns otherwise). `-Wno-unused-parameter` on the Objective-C only (AppKit's
delegate and action signatures). A build has zero warnings; `test-build`
enforces it.

**Code signing.** The first "Apple Development" identity in the keychain,
else ad hoc; `make CODESIGN_IDENTITY="…"` overrides. `build/codesign.id`
records the identity so a change forces a re-sign.

## The names

`src/identity.h` is the one place the app's names are spelled: the
display name, the bundle name (`camb.io_db_mgr`), the bundle identifier, the
icon file, the lock file, the Info.plist build-date key. The Makefile
reads them out of it (`ident`); `resources/Info.plist` holds only what is
constant, the rest is written by `plutil` at bundle time. `VERSION` is
`CFBundleShortVersionString`.

## Tests

Every test runs with `CAMBIO_SHARED_DB` set by the Makefile to a file
under `dev/test/<test>/`, and refuses to run otherwise (`tests/check.h`),
so nothing can touch the per-user database. `tests/check.h` is the
harness: `check`, `check_result`, `check_sandbox`, `check_pump` (the main
run loop, which drains the main dispatch queue the passes hand back to).

| Test | Covers |
|---|---|
| `unit/cdb_test` | `src/cdb.[ch]`: creating a database where none is and adopting one that is; the counts; size and SHA-256 of a file (a known digest, an unreadable file); the integrity check on a sound, a foreign and a missing file; the words of every verdict; a backup pass (made, the UTC name, nothing due, retention, a missing file); the policy values and their clamps; who has the file open (this process, while its connection is); what a crash leaves (log content, a live and an expired backup claim, an unfinished backup folder) and its words; path containment; the stamp |
| `features/service_test` | `src/service.[ch]`: start (created, the launch check lands on the main thread with the probe's size and digest), the one-copy lock, the first poll's backup, Back Up Now, a check that cannot run reaching the problem hook, the peer scan landing with this process listed, and a stop under a running pass that reports to no one |
| `ui/ui_check` | the window on a scratch database: titled, no header bar, every line present and filled once the check lands, the backup line's folder and warning, the Connections lines; light and dark snapshots; the window released on close |

## Layout

```
src/
  identity.h       the names
  look.h           sizes, spacing, timings
  cdb.[ch]         the facts and passes, plain C: open/adopt/create, counts,
                   size, SHA-256, the check, a backup pass, the policy, who has
                   the file open, what a crash leaves, words
  service.[ch]     the daemon: the main connection, the verdict, the passes on
                   workers (libdispatch), the backup poll and the daily check
  db_window.[hm]   the window under the coin
  app_delegate.[hm] the menu bar item, its menu, the signals, the lifetime
  main.m
icons/coin.png     the artwork (the .icns is built from it)
resources/Info.plist
tests/             check.h, unit/, features/, ui/
```

## Platform notes

- arm64 and x86_64 macOS 14+; not sandboxed (the database may be
  anywhere local; a lock file beside it).
- `LSUIElement`: no Dock icon, no menu bar menus. A minimal main menu is
  still built so that ⌘Q, ⌘W and the Edit shortcuts work in the window.
- The login item is `SMAppService.mainAppService` (macOS 13+); the
  registration belongs to the bundle that made it.
