# aw-watcher-switch

A local-only Nintendo Switch sysmodule for Atmosphere. It reads the OS play log (`pdm:qry`) and appends foreground play sessions to `sessions.jsonl` on the SD card.

Status: builds. It has not run on hardware. Firmware 23.0.1 with Kefir is untested, so there is no install or boot claim.

## What it does

- Polls the play log every 5 s, tracks at most one open session, writes one JSON line per closed session.
- Survives restarts through a log cursor and two state slots (see `docs/FORMAT.md`).
- Optional diagnostic mode writes the raw play-log entries to `diag.jsonl` so the open questions below can be answered from real data.

## What it never does

- No networking, no upload, no ActivityWatch client, no server. The NPDM allows only `fsp-srv`, `pdm:qry`, `time:u`, `set:sys` and 18 syscalls (`nx/allowed-syscalls.txt`). The build fails if network symbols are linked or if the JSON, the packed NPDM or the linked stubs grant anything else (`nx/check-no-net.sh`).
- No game names, no account attribution, no history import.
- No automatic installation or changes to system settings. Installing is a manual copy.

## Data on the SD card

Directory `/config/aw-watcher-switch/` (a convention, not a platform guarantee):

| file | content |
|---|---|
| `sessions.jsonl` | session and gap records, one JSON object per line |
| `state.a`, `state.b` | cursor and open-session state, two alternating slots |
| `diag`, `diag.jsonl` | diagnostic flag and output, only when you enable it. The file stops at 2 MiB total across boots and gets one final `trunc` line |

Privacy:
- `sessions.jsonl` is your play history: title ids and timestamps.
- `diag.jsonl` is more sensitive. It holds raw entries for every event type, including account events that contain raw account user ids. Keep it local, do not post it, and delete it after verification.
- Everything stays on the SD card. The host replay tool (`make replay`) only reads local files.

## Experiment boundaries

Sleep accounting is a best-effort guess until validated on hardware (see Hardware gates in `docs/INSTALL.md`). Do not treat durations as accurate.

| id | unverified |
|---|---|
| E1 | meaning of `power_state_change.value` |
| E2 | whether sleep time leaks into durations, meaning of network timestamp 0 |
| E3 | which `log_policy` values count as play time |
| E4 | program id word swap |
| E5 | whether sleep emits an out-of-focus event first |
| E6 | `end_entry_index` semantics, log capacity, persistence across reboot |

Each session record carries a confidence label in `sem` (see `docs/FORMAT.md`):

- `ok` means the tracker closed the session on observed events with both clocks present. It is software confidence, not verified accuracy.
- `exp` means the end depends on an unvalidated assumption: E1, E2, or a lost/expired log.
- `unc` means the end is uncertain: power/Initialize, reset or expiry closes, superseded sessions, and closes after an observation gap. They use the last confirmed observation, so `eu`/`en` can be `null`.

Do not sum any of these as an accurate playtime total. Sleep accounting is unverified, and `unc` records are deliberately conservative.

## Permissions

Deliberately narrow, with one gap:

- Services: `fsp-srv`, `pdm:qry`, `time:u`, `set:sys` only.
- SVCs: the 18 the linked binary actually calls (thread sync, IPC, own-process memory, sleep, break, info). No register read/write, no debug group, no process creation/termination, no secure monitor, no device address space, no heap or IO mapping. `nx/check-no-net.sh` decodes the packed NPDM and compares it to `nx/allowed-syscalls.txt`.
- Gap: `filesystem_access.permissions` is still the all-ones template value and the handle table is 1023. Narrowing either is unverified, so it stays wide and is disclosed here instead of being presented as a tight sandbox.

## Build

Needs Docker only. Nothing is installed on the host.

```sh
nx/build.sh
```

Uses the image pinned by digest in `nx/build.sh` (see `BUILD-ENV.md`), runs as your user, builds in a container copy, and writes only to `dist/`:

- `dist/sdcard/atmosphere/contents/4200000000415753/exefs.nsp`
- `dist/sdcard/atmosphere/contents/4200000000415753/flags/boot2.flag`
- `dist/aw-watcher.elf`, `dist/aw-watcher.npdm`
- `dist/build-info.txt` (sizes, sha256, decoded kernel access, check results)

Tests:

- `make test` for the core (host, sanitizers).
- `nx/test.sh` for the platform error/retry policy (host, sanitizers, no Docker).

Offline replay of a captured `diag.jsonl`: `make replay`, then `build/host/replay [--src live|dump|auto] [--watched|--unwatched] FILE...`.

Install and hardware verification: `docs/INSTALL.md`.
