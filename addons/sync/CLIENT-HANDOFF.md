# Sync client (owner A) handoff

Status: implemented and host-tested. Not run on hardware. Nothing committed.

## Files (all under `addons/sync/`)

1. `include/sync_paths.h`, `sync_plat.h`, `sync_engine.h`: PLAN section 5 verbatim. The appended private part of `sync_engine.h` defines `struct SyncEngine` (15,744 bytes, no heap, platform allocates it statically). Platform code only calls `sync_cfg_parse`, `sync_init`, `sync_run_once`.
2. `src/`: `sha256`, `cfg`, `ckpt`, `spool` (scan, batch cut, snapshots, anchor checks), `http` (request, ack binding), `engine`. About 840 non-test lines. Links only `core/src/util.c` (`crc32_of`, `SBuf`), unchanged.
3. `Makefile`: `make -C addons/sync test` (ASan + UBSan, `-Werror`), `sync_host`, `clean`. Output in `build/addon-sync/`.
4. `tests/`: `test_sync.c`, `fakes.c/h` (in-memory platform, fault injection, fake receiver with PLAN section 4 rules), `posix_plat.c/h` (real files, real nonblocking TCP with absolute deadlines), `sync_host.c`.

`sync_host`:

```
sync_host --root DIR --config FILE [--once] [--connect IP:PORT]
sync_host --scan JOURNAL          # prints "ep ev sub" for every CRC-valid line, sorted (matches --dump-keys)
```

Exit codes with `--once`: 0 idle, 2 retry, 3 blocked. `--root` maps SD paths under DIR. `--connect` replaces the connect target at the platform layer only, so loopback E2E works while the engine still enforces `100.64.0.0/10` from the config. The caller picks the root, so `config.txt` is the host file given with `--config`, and `journal=` / `state_dir=` keys are optional (defaults are the SD paths under DIR).

## Behavior that differs from or sharpens PLAN

1. **Snapshots before the exchange (plan correction).** The scan keeps the exact bytes it read: the first `min(64,end)` bytes and the last `min(64,end)` bytes ending at the batch end. Their SHA-256 values are computed before `exchange`. After a verified ack the engine re-reads those two ranges and compares to the snapshot hashes. Match: the checkpoint is written with the snapshot hashes (never recomputed). Mismatch, short file or missing file: checkpoint reset to offset 0 and replay (`resets` counter). Read error: RETRY, checkpoint unmoved. Tail bytes before the scan start come from bytes verified against the stored checkpoint hash, so no unverified byte enters a hash.
2. **Checkpoint advances only on a verified ack.** No checkpoint-only advance for trailing skipped lines. Trailing garbage after the last valid record is rescanned each run and shown in `skipped_lines` / `skipped_bytes` but never acked past. Replay after a reset recounts skipped lines.
3. **No `skipped.log`.** Counters only (`skipped_lines`, `skipped_bytes`, `held_bytes`, `resets` in `status.json`). `SYNC_NAME_SKIPPED` stays in the frozen header, unused.
4. **Unterminated lines.** CRC-valid last line without newline is sent with a newline added to the body, the cursor stays on the original bytes (end of line, no newline). The watcher's later fence `\n` is a blank line. Unterminated invalid tail is held (`held_bytes`), including one that outgrows 384 bytes. A terminated long line of any length is skipped and counted. Valid lines must be printable ASCII (0x20 to 0x7e) so one control byte cannot make the server answer 400 and block everything.
5. **Read-only journal and write guard.** The only write call is `st_write` in `engine.c`. It requires valid paths, refuses anything not directly under `state_dir`, anything under `/config/aw-watcher-switch`, and the journal path. `sync_cfg_parse` and the per-run check reject: non-absolute paths, `..`, `.`, empty components, odd characters, `state_dir` inside the watcher directory (case-insensitive, component-wise, so `/config/aw-watcher-switch-sync` is fine), `state_dir` equal to the journal directory, a journal inside `state_dir`. A config that fails the path check gets no writes at all, not even `status.json`.
6. **Errors on data reads are never "no journal".** Only `file_size` returning `-ENOENT` means `no_journal`. Any `read_at` failure, including `-ENOENT`, is RETRY.
7. **Ack parsing.** Accepts `HTTP/1.1` or `HTTP/1.0` (Python `http.server` defaults to 1.0). Requires one `Content-Length` of at most 512, no `Transfer-Encoding`, body length exactly equal to it (one trailing `\n` tolerated), the exact prefix of PLAN section 3, `k+j==n`. Statuses 400/401/403/409/413 are BLOCKED, everything else that is not a verified 200 is RETRY. 409 stores a short key (`ep/ev/sub` or `batch`) in `status.json` `conflict`.
8. **Net lifecycle.** `net_open` happens lazily before the first exchange of a run, `net_close` once on every return path after a successful open (wrapper `sync_run_once`). Idle runs open nothing.
9. **Config reload.** The engine does not read files for config. To pick up a fixed `config.txt` without reboot, the platform calls `sync_cfg_parse` and `sync_init` again at the start of each loop (state is on disk, backoff restarts).
10. Delays: IDLE 30 s, MORE 1 s, RETRY backoff 5 s doubling to 10 min, BLOCKED 30 min.

## Evidence

`make -C addons/sync test` (ASan + UBSan, `-std=c11 -Wall -Wextra -Wpedantic -Werror`):

```
../../build/addon-sync/test_sync
16312 checks
ALL TESTS PASSED
```

Covered: SHA-256 vectors (empty, `abc`, 56-byte two-block, 1,000,000 `a`, streaming) and CRC vector; config accept/reject table (hostnames, `0.0.0.0`, LAN, `100.63.255.255`, `100.128.0.0`, duplicate/unknown key, CR, NUL, oversize, bad UUID/token, `state_dir` inside the watcher dir in four spellings, traversal, relative path); good upload, status file; 100-record multi-batch; 40-batch cap then continuation; lost ack with byte-identical resend; 13 non-ack responses (wrong id, sha, count, new+dup sum, 500, truncated, oversize length, extra bytes, stale ack, no length, chunked, garbage, dropped) leave the checkpoint unmoved; 5 blocking statuses; backoff curve and reset; overlap dedup (journal growth, lost checkpoint with record-level dedup); byte-flip sweep over every byte of both checkpoint slots plus both-slots-bad and one-slot-missing; partial lines, CRC-valid unterminated last line, fence append, long unterminated tail; garbage between valid records, 3,000-byte line across window refills, CR line, trailing garbage; truncation, same-length rewrite in tail and in anchor, longer replacement, missing journal, `-EIO` and `-ENOENT` data-read errors; journal rewritten before the exchange, tail rewritten after commit, truncated during exchange, appended during exchange; 409 record conflict from edited journal; no write outside `state_dir`, no write at all on a bad config; deadline jump past the run limit; failed checkpoint writes; fault sweep (`-EIO` and `-ETIMEDOUT` injected at every failable platform call of a 70-record run with garbage, plus lost responses): server ends with exactly the 70 valid records, the checkpoint never passes an unacked record, `net_open`/`net_close` balanced.

Mutation check: disabling the post-ack snapshot verification makes 4 race checks fail (reverted).

Real HTTP E2E, `sync_host` (real files and sockets) against `server/awsync_server.py --allow-loopback`, journal built from `tests/fixtures/synthetic-diag.expected` plus garbage and a torn tail:

```
first run:      done/ok http=200 offset=1073 acked=5      exit 0
--scan vs --dump-keys: identical (5 keys)
second run:     done/idle                                  exit 0, still 5 rows
append 2 + garbage line: acked=7                           exit 0, 7 rows
server stopped, 1 record appended: send/retry errno=ECONNREFUSED  exit 2
server restarted: acked=8                                  exit 0, 8 rows
journal rewritten with changed records: anchor reset (resets=1), replay,
  server 409 conflict "2/50/1": ack/blocked                exit 3, DB unchanged at 8 rows
```

Checks: `git status` shows only `addons/` untracked, `git diff --stat -- core nx tests tools Makefile docs` empty, S1 grep finds nothing.

## Limitations

1. Only the first and last 64 bytes of the acked range are verified. A rewrite that preserves both and changes the middle is not detected. The server's record dedup returns 409 if such a rewrite later collides, it never overwrites.
2. A journal rewritten with different bytes under the same keys ends in BLOCKED by design (R6). Recovery is a new device UUID.
3. Skipped-line counters are since boot and recount after a reset replay.
4. `status.json` is rewritten once per run, not per batch. It is best effort.
5. Scan cost: a held or trailing garbage region is re-read every run (30 s) until a valid record follows or the line is terminated.
6. Not verified: FAT share mode with the live watcher (R9), Switch build, sleep/wake. The host platform cannot exercise them.
7. `sync_host --connect` is a test aid. It bypasses nothing in the engine, but the real platform must not offer it.
8. `sync_host` assumes a 1 KiB `config.txt` at most (engine limit is 512).
