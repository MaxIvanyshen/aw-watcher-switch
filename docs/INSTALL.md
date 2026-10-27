# Install and verify

Not tested on hardware. Firmware 23.0.1 with Kefir has not been tried. Nothing here is a claim that it boots.

## Install (manual)

There is no installer. The build never touches your console or SD card.

1. `nx/build.sh`. Check `dist/build-info.txt` says `check-no-net: OK`.
2. Power the console fully off (not sleep).
3. Mount the SD card on a computer.
4. Copy the contents of `dist/sdcard/` to the SD root. This adds `atmosphere/contents/4200000000415753/exefs.nsp` and `flags/boot2.flag`.
5. Put the card back and boot.

Remove: power off, delete `atmosphere/contents/4200000000415753/` from the SD card. Recorded data in `/config/aw-watcher-switch/` stays until you delete it.

Boot behavior: Atmosphere starts the module once at boot because of `boot2.flag`. The module does not crash or exit to get restarted, and it does not retry by aborting.

What is retried, with 1 s to 30 s backoff and no permanent exit: `smInitialize` and `fsInitialize` (so a failed early-boot service manager is retried, not fatal), SD mount, the data directory, `set:sys` version, `time` and `pdm:qry`, and `watcher_init` when state or the journal cannot be read yet. If the SD card becomes unavailable at runtime, the module remounts after 3 consecutive failed writes.

Persistence behavior on failure: a failed append leaves the event unconsumed in the OS play log and the cursor does not advance. Unconsumed events are only lost if the OS log itself expires them, which produces a `gap` record.

## Permissions granted

- Services: `fsp-srv`, `pdm:qry`, `time:u`, `set:sys`. Nothing else.
- Syscalls: 18, the set the linked binary calls (own-process memory, thread sync, IPC, sleep, break, info). No register read/write, debug syscalls, process creation/termination, secure monitor, device address space, heap or IO mapping. `nx/check-no-net.sh` decodes the packed `main.npdm` and fails the build if the JSON, the packed NPDM or the linked stubs differ from `nx/allowed-syscalls.txt`.
- Still broad, and not a sandbox: `filesystem_access.permissions` is the template's all-ones value and the handle table is 1023. Narrowing them is unverified without a boot test, so this build does not do it.

## Diagnostics

Enable (no reboot needed, checked every poll):

1. Power off, mount the SD card on a computer.
2. Create an empty file `/config/aw-watcher-switch/diag`. To also dump everything still in the OS play log once, write the text `dump_all` into it.
3. Boot, use the console as in the scenario below, power off fully.
4. Copy `diag.jsonl` and `sessions.jsonl` back to your computer.
5. Delete `diag` (and `diag.jsonl` when finished). `diag.jsonl` has raw account events with raw account ids. Keep it private.

Diagnostic output stops at 2 MiB per `diag.jsonl` file in total, across boots, and gets one final `trunc` line. Delete the file to start a new session. A write failure turns diagnostics off for that run and never affects recording.

Offline check: `make replay` builds the host replay tool, then `build/host/replay [--src live|dump|auto] [--watched|--unwatched] FILE...` (it also reads stdin). It runs the real tracker over raw `diag.jsonl` lines and prints the sessions it would have recorded. It reads local files only. `--src live` assumes continuous observation, `dump` assumes none; the default is chosen by input. Its output is an investigation aid, not a byte-exact journal recovery.

## Hardware gates (user, not agents)

All pending. None are claimed.

| gate | check | decides |
|---|---|---|
| H1 | Diag + `dump_all`. Launch a game, press power (sleep), wake, play, quit. Read the `raw` lines | E1, E5 |
| H2 | Compare `steady` timestamps across the sleep with real elapsed time | E2 |
| H3 | Reboot with a game open and with it closed, then read the log | log persistence, shutdown events |
| H4 | Check one known title id in `pid` | E4 |
| H5 | Read the `range` and `probe` lines | E6 |
| H6 | Network off, check whether `sn` is 0 | E2 |
| H7 | Boot with the nsp installed: does the process start and does data appear in the data directory. Title id collision | title id `0x4200000000415753` |
| H8 | Dropped as a hardware gate. Never remove the SD card while the console is on. Power off fully first | nothing here is claimed for the platform remount path |

What H8 used to cover, split by what actually exists:

- Core persistence and backoff on I/O errors are host-tested with fake adapters (`make test`): append failures, fsync failures, injected errnos, crash at every write. This covers the core only.
- The platform path is untested: the 3-consecutive-write-failure remount threshold (`nx/source/policy.c`, host-tested in `nx/test.sh` as a unit), and `fsdevUnmountDevice`/`fsdevMountSdmc` themselves (not testable on the host). A real SD failure on hardware is unverified.

After H1 to H6 the power policy and `sem` labels get a small follow-up. That is outside this build.
