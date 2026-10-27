# Sync add-on: handoff entry

## Current status

The uploader and receiver are implemented. The dispatch instructions below are historical; current behavior and limitations are in `README.md`, the three component handoffs, and the review reports.

- Main reran the portable suite: 16,312 checks passed with ASan/UBSan.
- The 24 receiver tests passed on both the Mac and Gerry (Python 3.14.7), with the real fixture present and no skipped tests on Gerry.
- `python3 addons/sync/e2e.py` passed real-client/real-receiver tests for delivery, replay, lost acknowledgement, conflict, and offline retry, using synthetic data on loopback.
- The pinned Switch build passed its service and packed-NPDM permission checks. NSP: 57,899 bytes, SHA-256 `a6d090093db8f79943cffe41db9cc79f002367ae72479b7f9375546325bac270`.
- A persistent user receiver is deployed on Gerry, bound only to its tailnet address on port 18766. An unauthenticated request returned 401. Its isolated synthetic-test database initially contains zero records; no real history has been sent.
- Watcher `core/`, `nx/`, root tests/tools/Makefile are unchanged. Switch installation, live file sharing, retry/replay and sleep/wake hardware gates remain pending.

## Historical dispatch

`PLAN.md` records the original contract. Implementation adjustments are documented in the component handoffs.

## Ground rules (all implementers)

1. Shared checkout, no branches, **no `git add` / `git commit` during the parallel phase.** The lead commits to `main` after the checks pass.
2. Touch only your own paths (PLAN section 2). `core/`, `nx/`, `tests/`, `tools/`, top-level `Makefile`, `docs/` are read-only for everyone.
3. No new dependencies. A: C11 plus `core/src/util.c`. B: libnx from the pinned image. C: Python 3 stdlib.
4. No secrets, real tailnet addresses or real device IDs in files. Examples use `100.64.0.1` and the zero UUID.
5. Do not run anything against the Switch or Gerry. Hardware gates (PLAN section 10) belong to the user.
6. Do not add features outside PLAN. Over budget? use the cut list in PLAN section 8.
7. Finish by running your own check, pasting its tail in your report, and listing files you created.

## Order

1. **A** writes `include/sync_paths.h`, `sync_plat.h`, `sync_engine.h` verbatim from PLAN section 5 in the first minutes, before anything else. B and C may start immediately from the PLAN text. B copies the headers once they exist on disk.
2. A, B, C run in parallel. They share no files.
3. Lead: merge check, write `e2e.sh`, run all checks (PLAN section 9), commit.

## A. Portable client (`addons/sync/{Makefile,include,src,tests}`)

Deliver: sha256, cfg parser, checkpoint slots, journal scan and batch builder with anchor checks, request builder and ack verifier, engine (`sync_init`, `sync_run_once`), status file, `skipped.log`.
Tests: `tests/test_sync.c`, `tests/fakes.c` (in-memory platform with per-call fault injection and a fake server implementing PLAN section 4 rules), `tests/posix_plat.c` and `tests/sync_host.c` (real files and real sockets, `--scan`, exit code 0 idle/ok, 2 retry, 3 blocked, loops until idle with `--once`).
Check: `make -C addons/sync test` prints `ALL TESTS PASSED`, ASan and UBSan clean.
Estimate: ~840 LOC plus tests, 5-6 h.
Watch: the unterminated-tail rules (PLAN 5.1.4), ack prefix exactness, checkpoint only after ack, anchor reset replays from 0 instead of skipping.

## B. Switch platform (`addons/sync/switch/`)

Deliver: `source/platform.c`, `source/main.c`, `Makefile`, `build.sh`, `sync.json`, `check-perms.sh`, `README.md`. Start from the probe in `/tmp/aw-watcher-switch-sync-prototype/prototype/background-probe` (copy, then adapt, do not edit the prototype tree).
Check: `sh addons/sync/switch/build.sh` passes with no warnings and `check-perms.sh` OK, prints sizes and sha256. Report the static-heap choice and `.bss` size. If A's sources are not complete yet, build against stubs and re-run when A reports.
Estimate: ~400 LOC, 2-3 h.
Watch: absolute deadlines on every poll/connect/send/recv, close the socket on every path, `net_close` must be safe to call twice, no `psc`/`time`/`nifm`, `fsdev` mount retries as in the probe, `mkdir` the data dir, `exchange` returns `-EMSGSIZE` when the buffer fills without close.

## C. Receiver (`addons/sync/server/`)

Deliver: `awsync_server.py`, `test_awsync_server.py`, `awsync.service`, `config.example.json`, `README.md` (deploy on Gerry with systemd, token creation, `ss -ltn` check, backup of the DB before the real-history gate).
Check: `python3 -m unittest discover -s addons/sync/server -v` all pass. Also `python3 awsync_server.py --config <test config> --allow-loopback` serving a manual `curl` POST using the PLAN section 3 shape.
Estimate: ~330 LOC, 2 h.
Watch: size check before reading the body, bearer compare in constant time, whole batch in one `BEGIN IMMEDIATE` transaction, ack bytes identical on replay (store them), bind refusal for anything outside `100.64.0.0/10` unless `--allow-loopback`, token file mode check.

## Lead after merge

1. `e2e.sh` (PLAN 9 E1), `make test` (watcher unchanged, W1), S1 grep.
2. One commit to `main` with only `addons/sync/**`.
3. Report to the user: gate list G0 to G6. G6 (real history) needs the record count, DB path and backup stated before the first send.

## Open questions for the user (none blocks the plan)

1. **Q1.** Reuse title ID `0x4200000000415755` (default, no tailscale-nx edit) or take a fresh ID?
2. **Q2.** Is systemd on Gerry fine, or does it only run Docker?
3. **Q3.** Port: default `18766` on Gerry's tailnet address unless something else owns it.
