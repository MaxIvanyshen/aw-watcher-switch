# Platform review (switch/main.c, platform.c, sync.json, build, perms)

Verdict: **APPROVED for hardware install.** No blocking bugs found. Nothing edited, no commits, no deploy.

Evidence: `sh addons/sync/switch/build.sh` rerun. Warnings-as-errors clean, `check-perms: OK (18 SVCs)`, text 79205 / data 12864 / bss 294784, exefs.nsp sha256 `a6d09009...5bac270` (57899 bytes), identical to PLATFORM-HANDOFF. `git grep` for tokens/tailnet IPs in `switch/` is empty. Only `addons/` is untracked.

## Checks

1. **Separate uploader, watcher unchanged.** Own title ID 4200000000415755 (check-perms asserts it), no `service_host`, `git status` shows only `addons/` new. OK.
2. **Write confinement.** `writable_path` (platform.c:32) requires the `/config/aw-watcher-switch-sync` prefix and rejects `..` and `//`. The watcher dir `/config/aw-watcher-switch/` does not match, so the journal is unwritable at the platform layer. The engine also guards in `st_write` (engine.c:26). `write_at` is called only from `st_write` (engine.c:31). OK.
3. **write_at.** `O_WRONLY|O_CREAT`, no `O_TRUNC`, `lseek` to the 64-bit offset, full write loop, `fsync` checked, `close` checked. Engine writes are fixed size (ckpt 128 bytes, status `STATUS_LEN` space padded), so no never-truncate leftovers. mkdir loop starts after `sdmc:/`, so it only creates components of the already guarded path. OK.
4. **read_at / file_size.** 64-bit `lseek`, loop to `cap` or EOF, `*got < cap` only at EOF, `close` checked, `*got` set only on success. `file_size` via `stat`, rejects non-regular. OK.
5. **Bounded net, absolute deadline.** `connect` is nonblocking, then `poll` with `left_ms(deadline)` (capped 60 s per poll and re-evaluated). The send loop and the recv loop recheck the deadline each pass. `SO_ERROR` is read after connect and on `POLLERR/POLLNVAL`. `POLLHUP` falls through to `recv` (0 means close). The fd closes on every path via `out:`. `*resp_len` is zero unless success. Full buffer then 1-byte probe gives `-EMSGSIZE` only if more data. The engine passes `dl = min(now+exchange, t0+run)`. No DNS or nifm. OK.
6. **Memory budget.** Static heap 256 KiB, bsd transfer memory about 0x1D000 (118 KiB) from that heap, 4 x 16 KiB-class buffers inside it, engine 15.7 KiB in `.bss`, main stack 0x8000 matching `main_thread_stack_size`. The session is closed after every run so the tmem is returned. Headroom is adequate. OK.
7. **Config reload / backoff.** Config is re-read each loop, and `memcmp` against the previous parsed config keeps the engine (and its in-memory backoff) unless the file changed. Invalid or missing config: no engine call, `SYNC_BLOCKED_MS` sleep. Setup retries are idempotent with a 10 s cap. `net_close` is idempotent and always called before sleeping. OK.
8. **Perms.** Services `fsp-srv`, `set:sys`, `bsd:u` only. JSON SVCs equal linked stubs equal decoded NPDM. Debug flags off. check-perms rejects nifm/dns/time/psc/applet/hid init symbols. OK.

## Non-blocking notes (hardware gates, already listed in PLATFORM-HANDOFF)

1. **N1.** `armGetSystemTick` across sleep is unverified (gate G5). A timeout error or a long delay is the worst case, no data risk because acks are verified against the snapshotted bytes.
2. **N2.** `poll`/`getsockopt` revents on bsd:u sockets, `fsync` and `mkdir` under fsdev, and the shared read of the watcher's open journal (G3) are untested in this binary. First boot should confirm `status.json` appears and a checkpoint slot is written.
3. **N3.** A `state_dir` outside `/config/aw-watcher-switch-sync*` passes engine validation but fails at the platform guard (known I3). The synthetic `-sync-test/state` path is supported.
4. **N4.** The check-perms journal grep only matches `write_at(` calls naming the journal on the same line. It is a lint, the real protection is points 2 and 3 above.
5. **N5.** `0xffffffffffffffff` filesystem permission mask is wide (PLAN O1), accepted.

Hardware install remains pending explicit approval.
