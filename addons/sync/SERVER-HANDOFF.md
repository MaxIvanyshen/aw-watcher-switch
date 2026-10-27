# Receiver (owner C) handoff

Status: implemented, all unit tests pass. Nothing committed, nothing deployed, nothing run against Gerry or the Switch.

## Files (all under `addons/sync/server/`)

1. `awsync_server.py`: 244 non-blank lines, stdlib only.
2. `test_awsync_server.py`: 24 tests, real `ThreadingHTTPServer` on `127.0.0.1` port 0.
3. `awsync.service` (user unit template), `config.example.json`, `README.md`.

## Test output

`python3 -m unittest discover -s addons/sync/server -v`

```
test_bind_rules  [TestConfig]
test_main_refuses_bad_bind  [TestConfig]
test_other_fields  [TestConfig]
test_token_rules  [TestConfig]
test_atomic_on_bad_last_line  [TestProtocol]
test_auth_and_device  [TestProtocol]
test_bad_lines  [TestProtocol]
test_batch_conflict_same_id_other_hash  [TestProtocol]
test_busy_db_gives_503_without_internals  [TestProtocol]
test_chunked_and_missing_length_rejected  [TestProtocol]
test_commit_ack_and_verbatim  [TestProtocol]
test_concurrent_batches  [TestProtocol]
test_exactly_384_ok_385_bad  [TestProtocol]
test_framing_and_header_errors  [TestProtocol]
test_max_records_ok  [TestProtocol]
test_overlap_counts  [TestProtocol]
test_oversize_rejected_before_body  [TestProtocol]
test_real_fixture_lines  [TestProtocol]
test_record_conflict_no_overwrite_and_rollback  [TestProtocol]
test_replay_returns_identical_ack_and_lost_ack  [TestProtocol]
test_restart_persistence_and_dump_keys  [TestProtocol]
test_stalled_body_times_out  [TestProtocol]
test_total_body_deadline  [TestProtocol]
test_truncated_body_commits_nothing  [TestProtocol]
Ran 24 tests in 1.520s
OK
```

A manual run with `--allow-loopback` and a curl POST returned `200` with the exact ack line and `--dump-keys` printed `1 1 1`. The DB file was mode 600.

## Behaviour notes for main and owner A

1. **Check order:** path (404), framing (400: missing, duplicate, non-digit `Content-Length` or any `Transfer-Encoding`), size (413, before reading the body), bearer (401), device (403), batch headers (400), body read, sha, line count, per-line validation (400), then the transaction.
2. **Line rules enforced:** starts `{"v":1,`, <= 384 bytes with the newline, suffix `,"c":"<8 lowercase hex>"}\n`, CRC32 of the bytes before the suffix equals `c`, no duplicate keys, no floats or `NaN`, `v`/`ep`/`ev`/`sub` must be exact ints (bools rejected), `ep` 1..2^32-1, `ev` 0..2^31-1, `sub` 0 for `gap` and 1 for `session`. Blank lines are a 400.
3. **Batch headers:** `X-Batch-Id` must match `<16 hex>-<digits>-<count>`, with the trailing count equal to `X-Batch-Count` (1..256) and to the real line count. The id's anchor and offset are opaque.
4. **Replay:** the ack is rebuilt from the stored `new`/`dup`, so a replay returns bytes identical to the first ack. Same id with a different hash or count is 409 `"key":"batch"`. A key with different bytes is 409 `"key":"ep/ev/sub"`, whole batch rolled back, including a conflict between two lines of one batch.
5. **Errors:** SQLite busy for more than 5 s and any other `sqlite3.Error` give `503 {"v":1,"error":"busy"}`. Error bodies carry no exception text. Logs carry client IP, device prefix (8 chars), batch id, counts and exception class only.
6. **Limits:** 16 concurrent requests (the 17th gets 503), 10 s per socket operation, 30 s total for the body, `Connection: close`, `Server: awsync`.
7. **No duration logic:** `dur`, `su`, `eu`, `ss`, `es` are never read or compared. A test stores `"dur":999999` and reads the same bytes back.
8. **`AWSYNC_TEST_DROP_RESPONSE`** (PLAN O2, G4) is implemented: when the env var is set the server commits and closes without answering. The unit file does not set it. It is a test hook that is live in production code.

## Deploy override applied (user systemd, not PLAN section 4.11)

1. Paths: app `~/.local/share/aw-switch-sync/awsync_server.py`, data `~/.local/share/aw-switch-sync/sync.db`, config and token `~/.config/aw-switch-sync/`. `~` is expanded inside the JSON config. Config path comes from `--config` or `AWSYNC_CONFIG`.
2. Unit: `ExecStart=/usr/bin/env python3 %h/.local/share/aw-switch-sync/awsync_server.py`, `ReadWritePaths=%h/.local/share/aw-switch-sync`, `Restart=on-failure`, `RestartSec=10`, `StartLimitIntervalSec=0`. Hardening: `NoNewPrivileges`, `PrivateTmp`, `ProtectSystem=strict`, `RestrictAddressFamilies=AF_INET AF_UNIX`, `RestrictNamespaces`, `LockPersonality`.
3. Dropped from PLAN 4.11 because a user manager cannot use them: `User=`, `StateDirectory`, `ConfigurationDirectory`, `IPAddressDeny/Allow`, `ProtectHome`. `After=tailscaled.service` is also impossible from a user unit, so a failed bind (tailnet not up yet) is retried every 10 s with no start limit.
4. Token file: owned by the running user, no group or other bits (mode 600 or 400), content `[A-Za-z0-9._~+-]{16,128}` after stripping whitespace.

## Open items for main

1. **R1.** The unit file was not run through systemd (written on macOS). Check `systemctl --user status awsync` after install. If the unit fails with status 218 or 226, user namespaces are unavailable: remove `ProtectSystem` and `ReadWritePaths` first.
2. **R2.** Port 18766 was not checked for use on Gerry. Main verifies it (`ss -ltn | grep 18766`).
3. **R3.** Slow-header trickling is bounded only by the 10 s per-read timeout, not a total deadline (the body has a 30 s total). Acceptable on a tailnet, add a total header deadline if that changes.
4. **R4.** Python 3.14 on Gerry is untested here. The tests ran on 3.15.0 (macOS).
5. The `Content-Length` > 65536 case answers 413 before auth, as PLAN section 4.4 specifies, so an unauthenticated client can learn the limit.
