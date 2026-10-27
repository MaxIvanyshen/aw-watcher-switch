# Sync add-on: production uploader plan

Status: plan only, nothing implemented. Basis: `docs/FORMAT.md`, `docs/SYNC-FEASIBILITY.md` (background probe PASS on this console), and the throwaway scaffold `prototype/background-probe` (commit `a794d59` of `/tmp/aw-watcher-switch-sync-prototype`, files `source/main.c`, `build.sh`, `check-perms.sh`, `probe.json`, `Makefile`).

Scope: a separate sysmodule that uploads the local `sessions.jsonl` to Gerry over `bsd:u` to a numeric tailnet IPv4, plus a small Python receiver with SQLite. Out of scope: ActivityWatch, dashboards, deleting or rewriting any local record, TLS, DNS, MagicDNS, any change to `core/`, `nx/`, `tests/`, `tools/` or the top-level `Makefile`.

## 1. Decisions

1. **D1. Separate title, read-only on the watcher's files.** The uploader never writes under `/config/aw-watcher-switch/`. Its own files live in `/config/aw-watcher-switch-sync/`.
2. **D2. Title ID `0x4200000000415755`** (the probe's ID, probe is retired and its `boot2.flag` is already removed). It is already in `mitm_titles`, so no tailscale-nx config edit. The ID is one constant in `switch/build.sh` and `switch/sync.json`. Fallback if a clean slate is wanted: pick another `42...` ID and add it to `mitm_titles`.
3. **D3. Plain HTTP/1.1, one TCP connection per request, `Connection: close`.** WireGuard encrypts. One `bsd:u` session per run, closed at the end of the run, so the uploader never holds a session while idle and never blocks tailscale-nx Stop (feasibility C5, U5).
4. **D4. Journal bytes are the payload.** The batch body is the exact CRC-valid journal lines, concatenated, each ending in `\n`. The client does not re-encode, time-stamp, sum or interpret anything. This is the "no inflated data" guarantee: a sleep or wake cannot change what is uploaded, because nothing is computed.
5. **D5. Two dedup layers, both on the server.** Batch layer: `(device, batch_id)` with a stored SHA-256, returns the stored ack on replay and `409` on a different hash. Record layer: `(device, ep, ev, sub)` with the stored line, skip identical, `409` on a different line. The whole batch is one SQLite transaction.
6. **D6. The client advances only on a verified ack** (section 6). Anything else leaves the checkpoint untouched.
7. **D7. Replay is the universal recovery.** Corrupt or missing checkpoint, replaced or truncated journal: restart from offset 0. The server's record dedup makes that safe. A record that collides with different bytes is a loud `409` and the client stops (BLOCKED), it never overwrites and never skips.
8. **D8. No PSC registration, no `time`, `nifm`, `sfdnsres`, `applet`, `hid`.** Same as the probe. Sleep/wake is handled by deadlines and reopening the session, not by notification.
9. **D9. Receiver deployed with systemd, no Docker.** Docker only adds a network-namespace problem for a tailnet-only bind. Revisit if Gerry has no systemd.
10. **D10. Build in the pinned devkitPro image** (`devkitpro/devkita64@sha256:1fc388c3...`, the one the watcher and probe use). `/tmp/tsnx/libnx` does not exist in this checkout (`/tmp/tsnx` has the tailscale-nx tree, `/tmp/libnx` has plain libnx). The probe built and ran on hardware with the image's libnx, so no custom socket library is needed.

## 2. Layout and ownership

```
addons/sync/
  PLAN.md  HANDOFF.md                  lead
  Makefile                             A   (host: test, sync_host, clean; build dir build/addon-sync)
  include/sync_plat.h                  A   frozen, section 5
  include/sync_engine.h                A   frozen, section 5
  include/sync_paths.h                 A   frozen, section 5
  src/sha256.c sha256.h                A
  src/cfg.c                            A
  src/ckpt.c ckpt.h                    A
  src/spool.c spool.h                  A   scan + batch build + anchor checks
  src/http.c http.h                    A   request build + ack verification
  src/engine.c                         A   sync_init / sync_run_once, status file
  tests/                               A   host tests, fakes, posix_plat.c, sync_host.c
  switch/source/main.c platform.c      B
  switch/Makefile build.sh sync.json check-perms.sh     B
  switch/README.md                     B   install, config, recovery
  server/awsync_server.py              C
  server/test_awsync_server.py         C
  server/awsync.service config.example.json README.md   C
  e2e.sh                               lead, after A and C are merged
```

Core reuse: A links `core/src/util.c` for `crc32_of` (read-only reuse, no change). No other watcher code is linked.

## 3. Wire protocol v1 (frozen)

Request:

```
POST /v1/batch HTTP/1.1
Host: <ip>:<port>
Authorization: Bearer <token>
X-Device: <device>
X-Batch-Id: <anchor16>-<offset>-<count>
X-Batch-Count: <count>
X-Batch-Sha256: <64 lowercase hex of the body>
Content-Type: application/x-ndjson
Content-Length: <n>
Connection: close

<count lines, each exactly as in the journal, each ending in \n>
```

1. `device`: `[0-9a-f-]{36}` UUID from config. Server accepts only devices listed in its config.
2. `anchor16`: first 16 hex chars of SHA-256 of the journal's first `min(64, batch_end_offset)` bytes. `offset`: decimal journal offset of the first byte of the batch range. `count`: records in the body.
3. The batch range `[offset, end)` may contain skipped (invalid) lines between valid ones. They are not sent. `end` is the offset after the last included line.
4. Limits: client `count <= 32`, body `<= 12288` bytes, each line `<= 384`. Server: body `<= 65536` (413 above, rejected before reading the rest), `count <= 256`.

Responses (all `Content-Type: application/json`, body `<= 512` bytes):

| status | meaning | client action |
|---|---|---|
| 200 | committed or exact replay | verify ack, advance |
| 400 | malformed batch, hash mismatch, bad line | BLOCKED |
| 401 | bad token | BLOCKED |
| 403 | unknown device | BLOCKED |
| 409 | batch id with other hash, or record key with other bytes | BLOCKED |
| 413 | too large | BLOCKED |
| 5xx, network error, timeout | transient | RETRY with backoff |

Ack body (exact bytes after the fixed prefix, single line):

```
{"v":1,"id":"<id>","sha256":"<hex>","count":<n>,"new":<k>,"dup":<j>}
```

The client builds the prefix `{"v":1,"id":"<id>","sha256":"<hex>","count":<n>,` from what it sent and requires it byte-for-byte, then requires `"new":<k>,"dup":<j>}` with `k+j==n`. Status must be `200`. Anything else is not an ack. 409 body: `{"v":1,"error":"conflict","key":"<ep>/<ev>/<sub>"}` or `"batch"`, logged to status verbatim.

## 4. Receiver (C)

1. Python 3 stdlib only: `http.server.ThreadingHTTPServer`, `sqlite3`, `hashlib`, `hmac`, `json`, `zlib`.
2. Config JSON, path from `--config`: `{"bind":"100.x.y.z","port":18766,"db":"/var/lib/awsync/sync.db","token_file":"/etc/awsync/token","devices":["<uuid>"]}`. The token is read from `token_file` (mode must not be group or world readable, else refuse to start). Nothing secret in the repo or the unit file.
3. Bind validation at startup: IPv4 inside `100.64.0.0/10` only. `0.0.0.0`, `::`, hostnames and anything else are fatal. Loopback is allowed only with `--allow-loopback` (tests, e2e).
4. Auth: `hmac.compare_digest` on the bearer token. Check order: size header (413), auth (401), device (403), then body.
5. Per-connection socket timeout 10 s, `Content-Length` required, body read bounded, `Connection: close`.
6. Per line validation: `\n` terminated, `<= 384` bytes, `json.loads` ok, `v==1`, `t in {session, gap}`, `ep`, `ev`, `sub` ints with `ep>0`, `sub in {0,1}`, CRC32 of bytes from `{` to just before `,"c":"xxxxxxxx"}` equals `c`. Any failure is 400 for the whole batch.
7. Schema (`PRAGMA journal_mode=WAL; synchronous=FULL; foreign_keys=ON`):

```sql
CREATE TABLE batches(device TEXT, batch_id TEXT, sha256 TEXT, count INT, new INT, dup INT, received_at INT,
                     PRIMARY KEY(device, batch_id)) WITHOUT ROWID;
CREATE TABLE records(device TEXT, ep INT, ev INT, sub INT, line BLOB NOT NULL, batch_id TEXT, received_at INT,
                     PRIMARY KEY(device, ep, ev, sub)) WITHOUT ROWID;
```

8. Handler: `BEGIN IMMEDIATE`. If `(device, batch_id)` exists: same hash returns the stored ack, different hash returns 409 `batch`. Otherwise for each record: existing row with identical `line` counts `dup`, different `line` rolls back and returns 409 `conflict` (the same rule applies to two lines with one key inside one batch), absent rows insert and count `new`. Insert the batch row, `COMMIT`, then answer. A crash before commit leaves nothing. A crash after commit but before the response makes the retry an exact replay.
9. `--dump-keys DEVICE` prints `ep ev sub` lines in key order, for the e2e comparison and for the real-history gate.
10. `received_at` is server time for audit only. It never feeds any duration or merge.
11. systemd unit: `User=awsync`, `StateDirectory=awsync`, `ConfigurationDirectory=awsync`, `After=tailscaled.service network-online.target`, `Restart=on-failure`, `RestartSec=10`, `NoNewPrivileges=true`, `ProtectSystem=strict`, `ProtectHome=true`, `PrivateTmp=true`, `ReadWritePaths=/var/lib/awsync`, `IPAddressDeny=any` plus `IPAddressAllow=100.64.0.0/10` (a defense in depth line, the bind check is the primary control). README lists the token command (`umask 077; openssl rand -hex 32 > /etc/awsync/token`) and the check `ss -ltn` showing only the tailnet address.

## 5. Frozen client contract (A writes these three headers verbatim first)

`include/sync_paths.h`:

```c
#ifndef AWSYNC_PATHS_H
#define AWSYNC_PATHS_H
#define SYNC_DATA_DIR      "/config/aw-watcher-switch-sync"
#define SYNC_PATH_CONFIG   SYNC_DATA_DIR "/config.txt"
#define SYNC_PATH_JOURNAL  "/config/aw-watcher-switch/sessions.jsonl"   /* same string as AW_PATH_SESSIONS, read only */
#define SYNC_NAME_CKPT_A   "sync.a"
#define SYNC_NAME_CKPT_B   "sync.b"
#define SYNC_NAME_STATUS   "status.json"
#define SYNC_NAME_SKIPPED  "skipped.log"
#endif
```

`include/sync_plat.h`:

```c
#ifndef AWSYNC_PLAT_H
#define AWSYNC_PLAT_H
#include <stddef.h>
#include <stdint.h>

/* Every function returns 0 or -errno. Paths are SD-root relative, the platform maps them (for example "sdmc:" prefix).
 * Every call is bounded. No function may block without a deadline. */
typedef struct SyncPlat {
    void *ctx;
    int (*file_size)(void *ctx, const char *path, uint64_t *size);              /* -ENOENT if absent */
    /* open, read at most cap bytes starting at off, close. *got < cap only at EOF. */
    int (*read_at)(void *ctx, const char *path, uint64_t off, void *buf, size_t cap, size_t *got);
    /* create if absent, write n bytes at off, flush and fsync, close. Never truncates. */
    int (*write_at)(void *ctx, const char *path, uint64_t off, const void *buf, size_t n);
    uint64_t (*now_ms)(void *ctx);                                              /* monotonic */
    int  (*net_open)(void *ctx);                                                /* socketInitialize on bsd:u, idempotent */
    void (*net_close)(void *ctx);                                               /* socketExit, idempotent */
    /* New TCP connection to ip:port, send all req bytes, read until the peer closes or resp_cap is full
     * (full without close returns -EMSGSIZE). Absolute deadline in now_ms units covers connect, send and recv
     * (-ETIMEDOUT). Platform closes the socket on every return path. */
    int (*exchange)(void *ctx, const uint8_t ip[4], uint16_t port, const void *req, size_t req_len,
                    void *resp, size_t resp_cap, size_t *resp_len, uint64_t deadline_ms);
} SyncPlat;
#endif
```

`include/sync_engine.h`:

```c
#ifndef AWSYNC_ENGINE_H
#define AWSYNC_ENGINE_H
#include "sync_plat.h"
#include "sync_paths.h"

#define SYNC_LINE_MAX            384
#define SYNC_BATCH_MAX_BYTES     12288
#define SYNC_BATCH_MAX_RECORDS   32
#define SYNC_MAX_BATCHES_PER_RUN 40
#define SYNC_EXCHANGE_TIMEOUT_MS 20000u
#define SYNC_RUN_TIMEOUT_MS      120000u
#define SYNC_IDLE_MS             30000u
#define SYNC_BACKOFF_MIN_MS      5000u
#define SYNC_BACKOFF_MAX_MS      600000u
#define SYNC_BLOCKED_MS          1800000u

typedef struct {
    uint8_t  ip[4];            /* must be inside 100.64.0.0/10 */
    uint16_t port;
    char     device[37];       /* lowercase UUID */
    char     token[129];       /* [A-Za-z0-9._~+-]{16,128} */
    char     journal[96];      /* default SYNC_PATH_JOURNAL */
    char     state_dir[96];    /* default SYNC_DATA_DIR */
} SyncCfg;

/* config.txt: LF lines "key=value", keys server (a.b.c.d:port, numeric only), device, token, optional journal,
 * state_dir. Unknown key, duplicate key, CR, missing required key, non-tailnet IP: -EINVAL. Total size <= 512. */
int sync_cfg_parse(const char *text, size_t n, SyncCfg *out);

typedef enum { SYNC_IDLE = 0, SYNC_MORE, SYNC_RETRY, SYNC_BLOCKED } SyncOutcome;
typedef struct { SyncOutcome outcome; uint32_t delay_ms; } SyncRun;

typedef struct SyncEngine SyncEngine;   /* defined in sync_engine.h by A below this line, ~14 KiB, platform allocates statically */
void   sync_init(SyncEngine *e, const SyncPlat *p, const SyncCfg *cfg);
/* One bounded pass: verify checkpoint and anchors, scan, send up to SYNC_MAX_BATCHES_PER_RUN batches over one
 * net_open/net_close pair, update checkpoint and status. Never loops past SYNC_RUN_TIMEOUT_MS.
 * The platform main loop is: for (;;) { r = sync_run_once(&e); sleep(r.delay_ms); }
 * IDLE: nothing pending. MORE: capped, more pending. RETRY: transient failure, backoff grows and resets on progress.
 * BLOCKED: server said no (400/401/403/409/413) or config invalid. */
SyncRun sync_run_once(SyncEngine *e);
#endif
```

A appends the `struct SyncEngine` definition to the header (private fields, platform treats it as opaque). B and tests only call `sync_init` and `sync_run_once`.

### 5.1 Client semantics

1. **Config and secrets.** `config.txt` on the SD holds server, device UUID and token. Nothing hard-coded: no host, no device, no token in source. Config is re-read on every run so a fix needs no reboot. Invalid config is BLOCKED, never a crash.
2. **Checkpoint slots.** `sync.a` and `sync.b` in `state_dir`, 128 bytes, little endian, alternate in place, written by `write_at` (fsync inside) only after a verified ack:

```
0   "AWY1"        4   u32 seq        8   u64 offset (first unacked byte)   16 u64 acked_records
24  u32 anchor_len (<=64)   28 u32 tail_len (<=64)
32  u8[32] anchor_sha (sha256 of journal bytes [0, anchor_len))
64  u8[32] tail_sha   (sha256 of journal bytes [offset - tail_len, offset))
96  zero padding to 124       124 u32 crc32 over bytes 0..123
```

   Load picks the CRC-valid slot with the highest seq, tie to A. Both invalid or absent: offset 0, empty anchor, replay. A failed slot write is non-fatal (the server already has the data, worst case is a replay).
3. **Anchor check** every run when `offset > 0`: `file_size >= offset`, `anchor_len` bytes at 0 hash to `anchor_sha`, `tail_len` bytes ending at `offset` hash to `tail_sha`. Any miss means the journal was replaced, truncated or rewritten: reset to offset 0, write the reset checkpoint, count `resets` in status, replay everything. A replacement journal is therefore never skipped. A missing journal (`-ENOENT`) is IDLE with status `no_journal`, checkpoint untouched. Read errors are RETRY.
4. **Scan.** Read from `offset` in chunks into the batch buffer. A line is terminated by `\n`. Classification:
   - blank (`\n` only): silent skip. This is the watcher's torn-tail fence.
   - valid: `<= 384` bytes with the newline, starts `{"v":1,`, ends `,"c":"xxxxxxxx"}`, `crc32_of` over the prefix matches `c`. Included in the batch.
   - unterminated valid CRC line at EOF: included (FORMAT section 2.4 calls it committed). The following fence `\n` is a blank line.
   - unterminated and not valid at EOF: held. Not sent, offset not advanced past it, retried next run. If it grows past 384 bytes without a newline it is classified `long` and skipped to the next `\n` once one exists, until then held.
   - terminated and invalid (`crc`, `shape`, `long`): skipped. The skip is recorded in `skipped.log` (one line `off=<u64> len=<n> why=<crc|shape|long> head=<first 48 bytes, non-printables as ?>`, append-only, capped at 64 KiB then a single `trunc` line) and counted in status. The offset advances past it only together with a verified ack of the batch containing it, or, when no valid record follows it, by a checkpoint-only advance (no network). Skipping is never silent: `status.json` always carries `skipped_lines`, `skipped_bytes`, `held_bytes`.
5. **Batch cut.** Stop at 32 records, or when the next valid line would exceed 12288 bytes. The range is `[offset, end)` where `end` is just after the last included line. Skipped lines after the last included line are left for the next batch. Deterministic: same file bytes and same offset always give the same body, so a retry after a lost ack resends identical bytes and the server answers with the stored ack. If the journal grew in between, the new batch has a different count, hence a different id, and the record layer dedups the overlap.
6. **Ack binding (D6).** After `exchange`, parse the status line (`HTTP/1.1 200`), require a `Content-Length` not above 512 and the body fully present, then require the exact ack prefix of section 3 for this batch (id, sha256, count). A mismatch of any field, a truncated response, a response for another batch, or `k+j != n` is a transient error (RETRY) and the checkpoint does not move. 4xx per the table is BLOCKED.
7. **Checkpoint advance.** After a verified ack: `offset = end`, `acked_records += count`, anchor and tail recomputed from the file (one `read_at` each), slot written. Only then the next batch.
8. **Backoff.** RETRY doubles from 5 s to 10 min and resets on any verified ack. BLOCKED waits 30 min and re-attempts (a fixed config or a resolved server state recovers without reboot). IDLE is 30 s.
9. **Deadlines.** One absolute deadline per exchange (20 s) and one per run (120 s) taken from `now_ms`. The engine checks the run deadline before each batch. A run that would exceed it returns MORE or RETRY. Sleep and wake: `now_ms` may jump or stall, an exchange in flight across a sleep simply fails or times out, the engine closes the session (`net_close`) and the next run reopens it. The engine holds no file handle between calls (every file call opens and closes).
10. **Memory.** One 12 KiB batch buffer, one 1 KiB response buffer, one 640 B request header buffer, one 384+ B line scratch, all inside `SyncEngine`. No heap use in A's code. Platform socket config: 1 session, 1 buffer set, 16 KiB TCP buffers (probe values). Static heap for the NSO: 384 KiB, to be confirmed by the build size report.
11. **Status file** `status.json`, fixed 320-byte space-padded single line rewritten at offset 0: `{"stage":"...","result":"idle|ok|retry|blocked|no_journal","errno":0,"http":0,"offset":0,"acked":0,"skipped_lines":0,"skipped_bytes":0,"held_bytes":0,"resets":0,"attempts":0,"conflict":""}`. Best effort.
12. **Never deletes.** There is no code path that removes, truncates or writes the journal. `write_at` is only ever called with `state_dir` paths.

## 6. Failure modes and where each is covered

| id | scenario | outcome | covered by |
|---|---|---|---|
| R1 | Watcher appends during scan | read stops at the last `\n` seen, unterminated invalid tail is held | A tests (partial line) |
| R2 | Ack lost after server commit | identical resend, stored ack | A fake server + C test |
| R3 | Wrong or stale response | checkpoint unmoved | A test (id, sha, count, status, truncation) |
| R4 | Checkpoint corrupt, both slots | replay from 0, server dedups | A test (byte flip sweep) |
| R5 | Journal truncated and rewritten, same length or longer | anchor or tail mismatch, reset to 0 | A test |
| R6 | Watcher state lost, epoch restarts, same keys, different bytes | server 409, client BLOCKED, nothing overwritten | A + C tests, e2e |
| R7 | Server or tailnet down | RETRY with backoff, no loss | A fake, hardware G2 |
| R8 | Sleep or wake during exchange | deadline or error, session reopened | hardware G5 |
| R9 | Watcher holds the journal open while uploader reads (FAT share mode) | read error is RETRY | **hardware G3, unverified** |
| R10 | Boot before tailscale-nx is ready | connect errors, backoff, reopen | hardware G1 (probe saw 4 retries) |
| R11 | Token leak via repo | config only on SD and in `/etc/awsync/token` | review, `git grep` check in A9 |

Resolving R6 is manual by design: assign a new `device` UUID on both sides (server `devices` list and `config.txt`), which starts a clean key space. No automatic merge.

## 7. Switch platform (B)

1. `switch/source/platform.c` (~260 LOC): `SyncPlat` over libnx `fsdev` stdio (`fopen`, `fread`, `fwrite`, `fflush`+`fsync`, `fclose`), `armGetSystemTick` for `now_ms`, `socketInitialize` with `BsdServiceType_User`, 1 session, 1 buffer set, 16 KiB TCP buffers, nonblocking connect/send/recv with a `poll` loop using the absolute deadline, `fcntl` and `getsockopt(SO_ERROR)` checked (lifted from the probe's `probe()`), `socketExit` on `net_close`. The probe's `attempt_deadline` / `remaining_ms` helpers are the model.
2. `switch/source/main.c` (~120 LOC): `__appInit` empty, in `main` set up `sm`, `fs`, `setsysInitialize`+firmware version, `fsdevMountSdmc` with the probe's retry loop, create `SYNC_DATA_DIR` (`mkdir`, ignore EEXIST), read `config.txt`, then the `for (;;)` loop of section 5. `svcSleepThread` for delays. No `psc`, `time`, `nifm`, `applet`, `hid`.
3. `switch/sync.json`: probe's NPDM with `name: "aw-sync"`, title `0x4200000000415755`, services exactly `fsp-srv`, `set:sys`, `bsd:u`, syscall list exactly what the linked binary uses. Filesystem permissions copied from the probe (`0xffffffffffffffff`), narrowing is an open item (O1).
4. `switch/check-perms.sh` = probe's script plus: fail on any `psc` / `nifm` / `sfdnsres` / `time:` service name or init symbol, fail if `SYNC_PATH_JOURNAL`'s directory is referenced by any write path (grep of the A sources for `write_at` callers, a coarse guard).
5. `switch/build.sh`: probe's script in the pinned image, compiles `../src/*.c` and `../../core/src/util.c` plus platform and main, outputs `dist/addon-sync/sdcard/atmosphere/contents/4200000000415755/{exefs.nsp,flags/boot2.flag}`, `sha256sum`, `aarch64-none-elf-size`. Warnings fail the build.
6. `switch/README.md`: install steps (copy `atmosphere/`, create `config.txt`, confirm the ID in `mitm_titles`, reboot), the status file, disable (delete `boot2.flag` or the title dir), recovery (remove ID from `mitm_titles`), and the "never remove the SD while on" rule.

## 8. Size and effort

| owner | scope | non-test LOC | agent time |
|---|---|---|---|
| A | headers 110, sha256 70, cfg 70, ckpt 110, spool 190, http 100, engine 190 | ~840 | 5-6 h plus tests |
| B | platform 260, main 110, NPDM json, build scripts (reused from the probe) | ~400 | 2-3 h |
| C | server 300, unit, example config, README | ~330 | 2 h |
| lead | `e2e.sh` 40, integration, commit | ~40 | 1 h |
| total | | ~1600 | |

Over the 1500 target by the headers and scripts. If A overshoots, cut in this order: `skipped.log` detail down to counters only (-40), status file down to a few fields (-30). Do not cut checkpoint verification or ack binding.

## 9. Automated checks (no hardware)

1. **A1.** `make -C addons/sync test` builds with `-std=c11 -Wall -Wextra -Wpedantic -Werror -fsanitize=address,undefined`. Tests (minimal, one file `tests/test_sync.c` plus a fake platform `tests/fakes.c`):
   - sha256 vectors (empty, `abc`, 1 MB of `a`), CRC check reuse.
   - cfg: accepts good, rejects hostname, `0.0.0.0`, `192.168.x.x`, `100.63.255.255`, `100.128.0.0`, duplicate key, unknown key, CR, oversized.
   - scan: valid, bad CRC, shape, blank fence, long line, unterminated valid, unterminated invalid (held then fenced), journal with garbage between valid records (valid after garbage is sent).
   - ckpt: byte-flip sweep over each slot, both slots bad, seq selection, fail-write is non-fatal.
   - anchor: truncation, replacement with longer, same length, missing file.
   - ack binding: wrong id, sha, count, `new+dup` sum, status 500, truncated body, oversize, extra bytes, 200 for another batch. Checkpoint never moves.
   - fault sweep: inject `-EIO`/`-ETIMEDOUT` at every platform call index of a multi-batch run against an in-process fake server that implements the section 4 rules. Invariant after eventual success: server holds exactly the valid records once, checkpoint offset never exceeds the last acked end.
   - fake server returns 409: outcome BLOCKED, checkpoint unmoved.
   - deadline: fake clock jumps past the run deadline, run returns without a further exchange.
2. **A2.** `build/addon-sync/sync_host --scan JOURNAL` prints `ep ev sub` for every CRC-valid line (used by e2e and the real-history gate).
3. **C1.** `python3 -m unittest discover -s addons/sync/server`: bind validator (reject `0.0.0.0`, `::`, hostnames, LAN IP, accept tailnet IP, loopback only with flag), token file mode refusal, 401/403/413, 400 for each bad line kind, atomic rollback (batch with a bad last record leaves zero rows), idempotent replay returns identical ack bytes, 409 batch, 409 record key (including inside one batch), persistence across server restart, `--dump-keys`.
4. **E1.** `addons/sync/e2e.sh` (lead): start the server on `127.0.0.1` with a temp DB, run `sync_host` (POSIX platform, real sockets, same engine code) against a journal built from `tests/fixtures/synthetic-diag.expected` (real CRC'd records) plus injected garbage and a torn tail. Assert: row count, `sync_host --scan` equals `--dump-keys`, second run adds zero rows, append more lines and rerun adds only those, kill the server mid-run then restart and finish, edit one record and rerun gives exit code BLOCKED with the DB unchanged.
5. **B1.** `sh addons/sync/switch/build.sh`: warnings-as-errors, `check-perms.sh` (services exactly `fsp-srv set:sys bsd:u`, JSON SVCs equal ELF stubs equal NPDM, no network-adjacent init), size report.
6. **W1.** Watcher untouched: `git diff --stat -- core nx tests tools Makefile docs/FORMAT.md` is empty and the existing `make test` still prints `ALL TESTS PASSED`.
7. **S1.** `git grep -nE 'Bearer [A-Za-z0-9]{16,}|token=[A-Za-z0-9]{16,}|100\.(86|116)\.' -- addons` finds nothing (no real token, no real tailnet address committed). Example files use `100.64.0.1` and `00000000-0000-0000-0000-000000000000`.

## 10. Hardware gates (user, not agents)

Run in order. None is claimed. G1 to G5 use a synthetic journal, so the real history is untouched until G6.

| gate | check | decides |
|---|---|---|
| G0 | Build `dist/addon-sync` (B1) and the server on Gerry (C1), `ss -ltn` shows only the tailnet address, token file 0600 | deploy ok |
| G1 | Install with `config.txt` pointing `journal=/config/aw-watcher-switch-sync/test.jsonl`, `state_dir=/config/aw-watcher-switch-sync/test`. Reboot. Copy a synthetic journal (3 valid, 1 bad CRC, 1 torn tail). `status.json` ok, server has exactly the 3 keys, `skipped.log` has 1 line, `held_bytes` > 0 | boot ordering R10, U1, U2, U3 |
| G2 | Stop the server, append a record, observe `retry`, restart server, record arrives, no duplicate | R7, backoff |
| G3 | Point the journal at the live watcher file in read-only mode and play a game for a few minutes while uploads run. Compare `sync_host --scan` of a copy of the journal (powered off, copied from a PC) with `--dump-keys`. Watch for watcher errors (`diag`) and missed records | R9 read sharing, **the main unknown** |
| G4 | Ack-loss simulation: block the receiver reply (stop the server after the DB commit is not possible by hand, so run the server with `AWSYNC_TEST_DROP_RESPONSE=1`, a test-only env that closes before responding) and confirm the replay dedups | R2 on real stack |
| G5 | Sleep with a pending record, wake, confirm upload resumes with no duplicate and no inflated durations server side (compare `dur`/`es`/`ss` bytes to the SD journal) | R8, U6 |
| G6 | **Real history.** The user has authorized the real upload. Before the first send the agent states the record count (`--scan` of a powered-off copy), the DB path and that the DB is backed up, then proceeds only on the user's go in that session. After: counts equal, key lists equal, second run adds zero. Overlay Stop still works (U5) | the real upload |

## 11. Open items

1. **O1.** Narrow the NPDM filesystem permission mask from `0xffffffffffffffff`. Out of this build, same as the probe.
2. **O2.** `AWSYNC_TEST_DROP_RESPONSE` (G4) is a test hook in C's server. It is honored only when the env var is set, and the unit file never sets it. If C prefers no hook in production code, drop G4 and keep the in-process R2 test.
3. **O3.** Concurrent open of `sessions.jsonl` by two processes (R9) is the one risk this plan cannot close on a host. The fallback if G3 fails: the uploader retries the open with a short delay inside the same run, and if still failing treats it as RETRY. No change to the watcher.
