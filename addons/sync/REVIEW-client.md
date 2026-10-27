# Sync client review

Scope: `include/`, `src/` (ckpt, spool, http, engine, cfg), tests, against PLAN and CLIENT-HANDOFF. Read-only review, no source edits.

Verdict: **Approve.** No blocking issue found.

## Checked

1. `make -C addons/sync test`: 16312 checks, all pass (re-run during review).
2. Watcher core/nx unchanged: `git status` shows only untracked `addons/`, no tracked file modified.
3. Cursor advance: `ck.offset`/`acked` change in one place (`engine.c`, after a verified ack and a successful post-ack snapshot verify). Every other exit (exchange error, bad ack, 4xx block, scan/verify read error) returns before touching `ck`.
4. Ack auth: `http_check` binds 200 to id, body sha, count, `new+dup==count`, exact `Content-Length`, no `Transfer-Encoding`. A mismatched or truncated response is RETRY, never an advance.
5. Failed checkpoint writes: `ckpt_save` failure leaves memory ahead of disk. Worst case is a replay after reboot, which the server dedups by key. No data loss.
6. Torn checkpoint: two slots, CRC, higher `seq` wins, write alternates slots. A torn write leaves the other slot valid.
7. Changed journal / partial append: snapshot hashes taken before the exchange, re-verified after the ack. Mismatch resets to 0 and replays (never binds the offset to new bytes). Unterminated invalid tail is held, not skipped. Unterminated valid tail is sent and the cursor stays at its end.
8. Config write guard: all writes go through `st_write` (path validity, under `state_dir`, not under the watcher dir, not the journal). Bad config gives no writes and BLOCKED. Journal is only ever read.
9. Deadlines: each exchange deadline is `min(now+20s, t0+120s)`; run loop checks the 120 s cap before every batch.

## Findings

F1 (low, non-blocking): `backoff_ms`/`attempts` are reset only after a verified ack. A RETRY followed by an IDLE run (for example the journal read error clears and nothing is pending) leaves the old backoff, so the next unrelated failure starts at the doubled delay instead of 5 s. Delay-only effect, no data impact. Fix if wanted: zero both in `idle()`.

F2 (known, documented, not a new bug): integrity checks cover only the first and last 64 bytes behind the cursor. An edit in the middle of already-acked data is not detected. Server-side record keys and the 409 conflict path are the backstop.

## Not done

No hardware run, no fuzzing. Findings F1 is by code reading only, not reproduced with a test.
