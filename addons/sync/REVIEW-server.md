# Receiver review (awsync_server.py)

Verdict: APPROVED for Gerry deployment. No blocking findings. 24/24 unit tests pass (re-run, Python 3.15 macOS). Server files untouched.

## Checked, no issue

1. Atomicity: `BEGIN IMMEDIATE`, batch lookup, per-record insert, batch row, `COMMIT` in one transaction. Any exception closes the connection, which rolls back. Record conflict (against stored or earlier line in same batch) rolls back everything.
2. Replay and lost ack: stored `new`/`dup` rebuild a byte-identical ack. Same id with other sha or count is 409 `batch`. Ack is sent only after `COMMIT`.
3. Durability: WAL + `synchronous=FULL`, per-request connection. Restart loses nothing committed. `AWSYNC_TEST_DROP_RESPONSE` commits first, then drops, so it models a lost ack correctly. Unit file does not set it.
4. Request checks: duplicate or non-digit `Content-Length` and any `Transfer-Encoding` give 400, size gives 413 before body read, bearer via `compare_digest`, device allowlist, batch headers strict regex, sha and count verified.
5. Line parsing: prefix, 384 cap, CRC over exact bytes, duplicate keys, floats and constants, bool rejection, ranges, `sub` tied to `t`, `c` field equals suffix. Stored bytes are the received bytes.
6. SQL: every statement is parameterized. The only formatted strings are the ack and the conflict key, built from validated values.
7. Token: mode/owner check on the open fd, charset check, never logged. Logs hold IP, 8-char device prefix, batch id, counts. Error bodies carry no exception text.
8. Unit: paths consistent with README, `ReadWritePaths` dir is created by the documented `mkdir`. DB dir 700, files 600 (umask 077).

## Non-blocking notes

1. N1. `ReadWritePaths` fails the unit start (status 226) if `~/.local/share/aw-switch-sync` does not exist. The README `mkdir` covers it. Do it before `enable --now`.
2. N2. `log_message` prints the raw request line, so a tailnet client can put control characters in the path into the journal. Cosmetic.
3. N3. The concurrency cap (16) applies only to POST. Other methods get 501 from the stdlib after header read. Header trickle is 10 s per read (acknowledged, tailnet only).
4. N4. Python 3.14.7 on Gerry: main re-ran all 24 tests, OK, no skips. Test-only ResourceWarnings not reviewed.
5. N5. Host-compat of the unit (R1 in handoff) can only be proven by `systemctl --user status awsync` on Gerry. If 218/226, drop `ProtectSystem`/`ReadWritePaths`.

## Not reproduced

No blocking issue found, so none reproduced.
