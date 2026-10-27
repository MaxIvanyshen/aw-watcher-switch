# Platform handoff (owner B)

Scope: `addons/sync/switch/` and this file. Nothing outside was touched. Not committed.

## Result

`sh addons/sync/switch/build.sh` passes against A's current source (headers final, `SyncEngine` 15744 bytes). Warnings are errors.

```
check-perms: OK (18 SVCs; services fsp-srv set:sys bsd:u; no nifm/dns/applet/hid/time/psc init)
   text	   data	    bss	    dec	    hex	filename
  79205	  12864	 294784	 386853	  5e725	aw-sync.elf
a6d090093db8f79943cffe41db9cc79f002367ae72479b7f9375546325bac270  exefs.nsp (57899 bytes)
```

Rebuild after any A change. The sha256 above is for the A source present at the time.

## Files

1. `switch/source/platform.c` (217 lines), `platform.h`: `SyncPlat` over fsdev POSIX calls and bsd sockets.
2. `switch/source/main.c` (79 lines): setup retries, config reload, run loop.
3. `switch/Makefile`, `build.sh`, `sync.json`, `check-perms.sh`, `README.md`.

## Decisions

1. **Paths.** From `addons/sync/switch` the watcher util is `../../../core/src/util.c` (PLAN said otherwise). `build.sh` copies `addons/sync` and `core/src` into `/tmp/w` keeping that relative layout. Include dirs: `../include ../src ../../../core/src`.
2. **Network.** `socketInitialize` with explicit `BsdServiceType_User`, 1 session, 1 buffer set, 16 KiB TCP buffers (probe values). `net_open`/`net_close` are idempotent. `main` also calls `net_close` after every run (idle, failure, success), so no session is held while sleeping.
3. **exchange.** Nonblocking connect, `poll` against the absolute `deadline_ms` (via `now_ms`), `SO_ERROR` checked after connect, `POLLERR`/`POLLNVAL` turned into `-SO_ERROR`, send loop handles short writes and EAGAIN, recv until close. `*resp_len` is zeroed first and set only on success. A full buffer is followed by a 1 byte probe read: close means success, more data means `-EMSGSIZE`. The socket closes on every path.
4. **Files.** `file_size` uses `stat` (no open). `read_at` loops `read` until `cap` or EOF, checks `close`. `write_at` opens `O_WRONLY|O_CREAT` without truncation, writes in a loop, `fsync`, checks `close`, creates missing parent dirs. Writes are refused (`-EPERM`) outside `/config/aw-watcher-switch-sync*` (so `-sync-test/state` works), with `..` or `//`. This is stricter than A's rule (state_dir anywhere that A allows): a `state_dir` outside that directory will fail checkpoint writes. The watcher journal directory is therefore unwritable at the platform layer too.
5. **Config reload.** Each loop: `read_at` of `config.txt` (513 bytes max), `sync_cfg_parse`. Unchanged config keeps the engine (and its in-memory backoff). Changed config calls `sync_init` again (backoff restarts, checkpoint reloads from SD, per CLIENT-HANDOFF). Missing or invalid config: no engine call, sleep `SYNC_BLOCKED_MS`, and no `status.json` is written (A writes none for a bad config either).
6. **Memory.** Static heap `0x40000` (probe value, kept: socket transfer memory is `0x1D000`, the engine is static `.bss`, not heap). Main stack raised from the probe's `0x4000` to `0x8000` for headroom against A's stack buffers (a `Batch` is on the stack). `.bss` 294784 bytes includes the 256 KiB heap array and the 15744 byte engine. PLAN's 384 KiB estimate is not needed.
7. **NPDM.** `sync.json` is the probe's with name `aw-sync` and the larger stack. Filesystem permission mask still `0xffffffffffffffff` (PLAN O1). The 18 SVCs match the linked ELF stubs and the decoded NPDM (hactool), as in the probe.
8. **check-perms additions.** Fails on `nifm*`, `psc*`, `sfdnsres`, `getaddrinfo`, `gethostby*`, `time*` init symbols, requires literal `BsdServiceType_User` in `platform.c`, and fails if any `write_at` call in `../src/*.c` mentions the journal.

## Not verified (hardware pending)

1. Boot ordering against tailscale-nx, MITM reopen after boot, bsd:u use from this title (the probe proved it for the same ID, not for this binary).
2. Sleep/wake: `now_ms` uses `armGetSystemTick`. Whether it advances during sleep is unknown. Deadlines assume it is monotonic. An exchange in flight across sleep is expected to fail or time out, then the next run reopens the session. Gate G5.
3. Read sharing with the watcher's open journal (R9, gate G3).
4. `fsync`/`O_CREAT` without truncate and parent `mkdir` under fsdev, and `poll` revents on bsd sockets, are untested in this binary. No on-console or emulator run happened.

## Integration issues for main

1. **I1.** A's config reload cost: backoff restarts only when `config.txt` changes. Acceptable, no A change needed.
2. **I2.** An invalid config produces no `status.json`. If visibility is wanted, A would need a status call that works without a valid `state_dir`. Not done.
3. **I3.** `state_dir` values not starting `/config/aw-watcher-switch-sync` pass A's validation but fail at the platform write guard. The synthetic config (`/config/aw-watcher-switch-sync-test/state`) and the PLAN G1 example both work.
4. **I4.** `dist/` is gitignored. Nothing to commit from the build.

## Commands

```
sh addons/sync/switch/build.sh                          # build + check-perms + size + sha256
git grep -nE 'Bearer [A-Za-z0-9]{16,}|token=[A-Za-z0-9]{16,}|100\.(86|116)\.' -- addons   # empty for switch/
git diff --stat -- core nx tests tools Makefile docs     # empty
```
