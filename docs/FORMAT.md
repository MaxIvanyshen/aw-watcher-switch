# Local data format and recovery

## 1. Files and scope

All paths are relative to the SD root under `/config/aw-watcher-switch/` (constants in `core/include/paths.h`). The platform creates the directory and maps paths to its mount prefix. There is no upload, network client, account attribution, game-name lookup or ActivityWatch dependency.

1. **F1. `sessions.jsonl`** is the append-only public journal. It is unbounded. One ASCII JSON object per line, at most 384 bytes including newline. Consumers must validate the CRC and ignore invalid lines, not stop at the first invalid line.
2. **F2. `state.a`, `state.b`** are internal binary recovery slots, not an export format. They snapshot cursor, open session, steady source and pending gap. Do not edit them while recording.
3. **F3. `diag` and `diag.jsonl`** enable and capture diagnostics. **Keep diagnostic files private.** Raw records include non-application events, including account identifiers in account-event bytes. Even ordinary session journals reveal title IDs and activity timestamps.

First run starts at the OS log tail. Existing history and an already-focused game are not imported. A game must produce a new InFocus event to open a session.

## 2. JSONL version 1

Every record has `v:1`, `t`, `ep`, `ev`, `sub`, and a final `c` field. Fixed field order makes encoding deterministic. The unique key is `(ep, ev, sub)`, ordered lexicographically. `ep` is a positive epoch counter, `ev` a signed OS log index, and `sub` distinguishes gap (0) from session (1). Session keys use the event causing the close. Synthetic loss/source-reset closes use the current cursor. An epoch-transition gap uses the committed cursor, which can be one past the triggering event.

Example session:

```json
{"v":1,"t":"session","ep":1,"ev":812,"sub":1,"pid":"0x0100000000001234","su":1700000000,"sn":1700000001,"ss":1000,"eu":1700000100,"en":1700000101,"es":1100,"dur":100,"why":"out_of_focus","pol":0,"sem":"ok","c":"8e0a19e1"}
```

### 2.1 Session fields

| Field | Meaning |
|---|---|
| `pid` | Program ID, `0x` plus 16 lowercase hex digits. Raw log's two little-endian u32 words are swapped when decoded (E4). |
| `su`, `sn`, `ss` | Start user, network and steady timestamps, unsigned integer seconds. |
| `eu`, `en`, `es` | End timestamps. For an inferred last-observed close, `eu` and `en` are null and `es` is the last confirmed steady observation. |
| `dur` | `es - ss`, in seconds, or null if either steady timestamp is zero or the pair is invalid. User/network clocks are never subtracted. |
| `why` | `out_of_focus`, `exit`, `superseded`, `power`, `init`, `clock_reset`, or `lost`. |
| `pol` | Policy byte from the opening InFocus event. Policy None (2) is ignored. Other policies are preserved, not interpreted as OS-counted playtime (E3). |
| `sem` | `ok`, `exp`, or `unc`, defined below. |

`sn=0` is passed through. It may mean unsynced, unverified (E2). User/network clocks can move backwards or jump without affecting duration arithmetic.

### 2.2 Semantics and conservative closure

1. **S1. `ok`** means matching explicit OutOfFocus/Exit endpoints, valid steady arithmetic, and no detected observation discontinuity. It does **not** mean hardware-validated playtime accuracy or prove there was no sleep (E2/E5).
2. **S2. `exp`** means an experimental power-event close. Any PowerStateChange closes under `WATCHER_POWER_POLICY_CLOSE_ANY=1`. The meaning of its value is unverified (E1).
3. **S3. `unc`** means uncertain. Superseded, lost, Initialize, clock-reset, invalid-clock pairs and observed closes after a long observation gap are never `ok`. Do not sum `unc` durations as accurate playtime.

The watcher touches a session's last-observed steady time only after reading the log successfully, or from processed events. A step over 30 seconds freezes that observation instead of counting suspended/off time as confirmed liveness. Persisted heartbeats are every 60 seconds, so a crash can lose up to 60 seconds of confirmed liveness. A restart from a stale heartbeat may therefore conservatively label a genuine continuing session `unc`.

**R8 protection:** Initialize and stream reset/expiry close at the last confirmed observation, not the later boot/reset timestamp. They cannot inflate the old session through a long off period. Such closes have `eu:null`, `en:null`, `sem:"unc"`. Clock-reset closes also use the old domain's last observation. Their numeric duration is only a last-observed interval in that old domain, not a duration across clock sources. This implementation keeps that lower-bound interval rather than the plan's unconditional null for clock-reset duration.

An explicit endpoint after an unobserved stall retains its actual timestamps and duration, but gets `sem:"unc"`. Without Initialize, reset, a source change or an observation gap, the core cannot prove a reboot/sleep occurred.

### 2.3 Gap fields

```json
{"v":1,"t":"gap","ep":3,"ev":20,"sub":0,"why":"log_dropped","from":10,"to":20,"c":"726a59a0"}
```

`why` is `first_run`, `state_lost`, `state_corrupt`, `log_dropped`, `log_reset`, or `steady_reset`. `from`/`to` describe cursor movement or the triggering index, not elapsed wall time. First-run/state-loss recovery starts at tail and uses tail for both fields. A steady event regression may have `ev=from+1` because the triggering event is already applied to the new tracker. Gaps identify unavailable or deliberately skipped history, not zero playtime.

### 2.4 CRC and torn lines

CRC is IEEE CRC32 (polynomial `0xedb88320`, initial/final complement, standard `123456789` vector `cbf43926`). It covers the bytes from `{` through the field immediately before the suffix `,"c":"xxxxxxxx"}`. The suffix and newline are excluded. `c` is eight lowercase hex digits. This is accidental-corruption detection, not authentication.

Torn/zero-filled tails are fenced by prefixing the next append with newline. No truncate or rename is needed. A complete CRC-valid line lacking its newline is considered committed and fenced before another record is appended. Readers must accept valid lines after garbage. A bounded 4096-byte tail scan locates the last valid key.

## 3. Persistence protocol

1. **P1. Ordinary events:** copy tracker, apply event, append any close record with checked write/flush/fsync/close, then advance cursor/tracker and persist a slot. Append failure leaves the event unconsumed. State-write failure backs off but replay remains safe while the OS log retains the events.
2. **P2. Epoch transitions:** append the old epoch's close **before** clearing its open session. Then fsync the new epoch/cursor with a pending gap in state, then append the gap. The pending marker survives a crash and is retried with key suppression. Persisting cleared state before the close, as an overly broad reading of PLAN's epoch order suggests, loses the session if power fails between them.
3. **P3. Recovery:** choose the CRC-valid slot with highest sequence, ties choose A. Suppress replayed keys less than or equal to the journal's last valid key. Before retrying a failed append, reread the tail because the bytes may be readable despite an error. A readable key alone is not proof of durability: after finding a valid tail record, recovery/retry must successfully call `append_line` with zero bytes on the journal before using its key for suppression or advancing state. That call still opens, flushes, fsyncs and closes the journal. A barrier failure keeps recovery/event consumption blocked and backs off. This also applies to process restart over an existing filesystem cache. Missing/invalid state with existing journal starts a new epoch at tail and emits a state-loss/corruption gap. State with an older epoch than the journal is not blindly promoted: treat it as lost and begin after the journal epoch. With no journal, absent/invalid state is treated as an interrupted first run.

The 104-byte state v1 slot is explicitly little endian: 12-byte header (`AWS1`, u16 version, u16 payload length, u32 sequence), 88-byte payload, u32 CRC over header+payload. It has no C struct padding on disk. Slots alternate in place. Unknown versions and invalid CRCs are rejected, not deleted. Non-ENOENT read errors defer recovery rather than masquerading as corruption. A subsequent successful slot write may overwrite a corrupt slot.

Guarantees assume a single writer, fsync honoring its contract, and corruption limited to the in-progress write. Arbitrary loss of both slots, removal/rollback of the journal, expiry before replay, or more than 4 KiB of trailing damage can lose history or defeat deduplication. This is not a filesystem repair tool. Epoch and slot sequence counters are finite u32 values and must not wrap.

Ranges are validated before use. Every poll uses a bounded tail probe to resolve inclusive/exclusive reported ends. When `start + total` agrees with either convention, that count disambiguates the first absent index, so an error on the last existing entry remains a real error. Errors at/beyond the established tail are accepted as EOF during normal reads and anchor checks as well as recovery; errors below it back off. A cursor beyond the true tail, including the one-entry-to-empty boundary, triggers reset. Cursor expiry/reset closes conservatively and starts a new epoch. Anchors are checked on startup and backwards range movement. An entirely rewritten, refilled log whose range never regresses while this process stays alive may evade detection. Backoff is 5 seconds to 5 minutes. At most 64 batches of 32 events are consumed per poll. Core memory is fixed, with no malloc and no event queue.

## 4. Diagnostics and offline replay

The flag is checked each poll. `dump_all` contents request one read-only log pass, then the flag is rewritten empty. The dump does not modify journal or state. Diagnostic writer failures disable diagnostics until restart and do not stop recording. Core bounds diagnostic output to 2 MiB per process plus one `trunc` marker. The platform writer must enforce the total file cap across restarts, since the frozen Fs interface has no stat/size callback.

Lines use `d` as type: `boot`, `range`, `probe`, `raw`, `tr`, `io`, `stats`, `trunc`. Platform may add `mem` records. Raw lines contain `k:"live"|"dump"`, index `i`, `hex` (112 hex chars), decoded type/policy/program/timestamps, `act` decision, and optional close `why`/`sem`. Diagnostic lines are not journal records and have no journal CRC. Account/nonapp data is unfiltered raw data and must remain private.

```sh
make replay
build/host/replay captured-diag.jsonl > replayed.jsonl
build/host/replay --src dump --unwatched captured-diag.jsonl > replayed-dump.jsonl
```

The CLI accepts at most one input file, or reads stdin when none is supplied. Multiple file arguments are rejected with exit status 2 before any output, so separate captures cannot accidentally reuse one output key space. Output is generic session/gap JSONL with CRC, summaries go to stderr. It links the real core decoder/tracker, not a second implementation. `auto` selects live if available, otherwise dump. Live defaults to `--watched`, assuming continuous observation between event timestamps. Dump defaults to `--unwatched`, exposing observation uncertainty. Override explicitly when investigating. Initialize never gets a fabricated continuous observation.

Replay is a diagnostic simulation, **not** a reconstruction of the exact on-device journal: heartbeat history, cursor gaps/source changes, boot epoch numbers and all filesystem transactions are absent from raw lines. Repeated identical raw entries in a bounded 256-index window are skipped. Changed/backwards indexes trigger a reset. An open session is closed using the previous consumed index plus one in the old epoch, then the epoch increments before the reset gap and new stream events. If that successor index cannot fit in s32, replay advances to a fresh epoch for the synthetic close as well. This keeps output keys strictly increasing instead of keying the old close at the lower reset index. Process separate capture files independently, and use `--src` to avoid mixing live and dump passes. Large repeated dumps and identical-event index reuse across resets can be ambiguous. No hardware fixture is claimed: `tests/fixtures/synthetic-diag.jsonl` is host-generated.

## 5. Hardware uncertainty

**Sleep accounting is a best-effort guess until validated on hardware (see Hardware gates in docs/INSTALL.md).**

1. **E1.** Power values and the close-any-power interpretation are experimental.
2. **E2.** Whether steady time advances during sleep and zero network time meaning are unverified.
3. **E3.** OS playtime treatment of log policies is unverified.
4. **E4.** Program ID word swap follows the libnx comment, pending known-title verification.
5. **E5.** Whether sleep logs OutOfFocus before power is unknown.
6. **E6.** Log end semantics, capacity, persistence across reboot and batching are unverified.

Host tests establish software behavior under fake adapters, not hardware correctness. Capture logs only through a safe, powered-off data transfer workflow. No live SD removal procedure is required or recommended here.
