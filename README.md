# TrpCleaner v3

Native Linux cache cleaner with an action menu, a read-only scan and a
Makefile. C++17, POSIX/Linux APIs and the C++ standard library. No Python,
ncurses, package downloads or root privileges required to run it.

## Build and install

From the extracted `TrpCleaner` directory:

```sh
make
make install
TrpCleaner --scan
TrpCleaner
```

`make install` installs the executable to `$HOME/.local/bin/TrpCleaner`.
Do not use sudo. If that directory is not already in your PATH, add this to
`~/.zshrc` (or `~/.bashrc`) and open another terminal:

```sh
export PATH="$HOME/.local/bin:$PATH"
```

You can also run it directly:

```sh
./TrpCleaner --scan
./TrpCleaner
```

The ZIP includes a prebuilt **Linux x86_64 static binary**. To build from
source on your own machine, use `make re` then `make install`. Building
requires a C++17 compiler and make. GCC 9+ is suitable for the included tests.
The Makefile defaults to a normal dynamically linked local build; the shipped
static binary was compiled separately with `LDFLAGS=-static`.

Optional commands:

```sh
make test                 # Temporary-fixture safety tests
make uninstall            # Remove ~/.local/bin/TrpCleaner
make fclean               # Remove local executables
make install PREFIX="$HOME/goinfre/tools"  # Install under tools/bin
```

`DESTDIR` and `BINDIR` overrides are supported for packaging.

## Use

```sh
TrpCleaner --scan                    # Read-only report
TrpCleaner                           # Campus-style terminal menu
TrpCleaner --plain                   # Numbered menu
TrpCleaner --clean firefox code pip  # Preselected groups, then confirmation
TrpCleaner --help
```

The interface follows Campus Store: orange header, numbered tabs, a highlighted
row, action keys and a persistent status bar. No checkbox selection is needed.

| Key | Action |
| --- | --- |
| 1–4 | All caches, Browsers, Dev tools, Apps |
| Up/Down or j/k | Move the highlighted row |
| Left/Right | Switch tabs |
| Enter | View category paths; on the first bulk row, preview cleaning all |
| c | Preview cleaning the highlighted category |
| a | Preview cleaning all supported caches across every tab |
| r | Rescan and refresh sizes |
| h | Show key help in the status bar |
| q / Escape | Quit or cancel the current preview |

In a cleanup preview, press `y` to confirm or `n`/Escape to cancel. Up/Down
scrolls the cache paths. After cleanup, the menu stays open, refreshes sizes,
and reports the deleted file count and allocated bytes. Details alone never
delete files. Each action takes a fresh snapshot before previewing.

Close the affected apps before confirming. Use a terminal at least 64 columns
by 18 rows; use `--plain` in small or basic terminals. The plain menu accepts
one category number, or `0` for all. Plain/CLI cleanup still requires typing
`CLEAN` after the preview. There is no automatic or forced cleanup.
Redirected input or output makes cleanup read-only. The program rejects root.

| Group | Included |
| --- | --- |
| firefox | `.cache/mozilla/firefox` |
| chrome / brave | Default/Profile cache directories and shader caches |
| code | VS Code Cache, CachedData, GPUCache, DawnCache, Code Cache |
| graphics | Thumbnail and known Mesa shader caches |
| npm | `.npm/_cacache` |
| pip | `.cache/pip` |
| flatpak | `.var/app/<app>/cache` |

Other content is outside the cleanup list, including Downloads, Trash,
projects, Git history, VS Code extensions, browser cookies/passwords/history/
bookmarks/session files, app configuration, Codex conversations, Docker,
Snap app data and Flatpak installations. Custom XDG paths and Snap Firefox
caches are not detected by this version. No migration is performed.

## File safety

The cleaner uses a fixed cache allowlist. It skips symbolic links, including
directories moved to goinfre, and skips foreign-owned entries, other
filesystems, hardlinks and special files. Each directory is opened with
`O_NOFOLLOW`. Cleanup checks the cache root inode and each file's inode,
device, size, modification/change time, owner and link count against the
preview before unlinking it. New or changed files are skipped. Empty cache
directories remain. Avoid concurrent storage migrations.

Deletion is permanent; applications rebuild caches, which can make the next
startup slower. Allocated-block estimates can differ from actual reclaimed
space, particularly if a running app holds deleted files open. The free-space
line describes the filesystem, not any separate account quota. Interrupted
cleanup retains already completed deletions.

The tests run entirely in temporary fixture directories and cover protected
files, scan-only behavior, symbolic links, hardlinks, replacement directories,
changed/new files, Flatpak boundaries and path traversal. Your real session
has not been accessed or cleaned during development.

Optional terminal interaction tests require Python 3 only for testing:

```sh
make test
python3 tests/test_tui.py
```

They verify menu navigation, one-action bulk cleaning, category cleaning,
details without deletion, cancellation, rescanning, small terminals and signal
restoration. The installed program has no Python dependency.
