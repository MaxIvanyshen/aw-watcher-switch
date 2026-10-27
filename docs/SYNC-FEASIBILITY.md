# Switch uploader over tailscale-nx: feasibility

Scope: a separate uploader add-on for the existing local watcher. The watcher stays generic and has no networking. Research only, no implementation.

## Sources

- tailscale-nx `v1.1.3`, commit `9deb05acdf06eeee1d089b668580da0e9d357a69` (cloned to `/tmp/tsnx`).
  - `switch/sysmodule/source/bsd_mitm.cpp` (all line numbers below)
  - `switch/sysmodule/source/bsd_mitm.hpp`
  - `switch/sysmodule/source/main.cpp` (config parsing, `bsd_mitm::Start` call)
  - `switch/sysmodule/res/config.ini.example`, `docs/SETUP.md`, `README.md`
  - `switch/mitm-test/source/main.c` (homebrew smoke-test pattern)
  - `patches/atmosphere-mitm-clone-no-abort.patch`
- libnx `b156b4774fb2df7b93402169509266da7547fc4c` (cloned to `/tmp/libnx`): `nx/source/services/bsd.c:218-228`, `nx/include/switch/runtime/devices/socket.h:6-9`, `nx/source/runtime/devices/socket.c:76,125-136`.

## Findings

### F1. How tailscale-nx hooks sockets
It installs an Atmosphere MITM on `bsd:u` only (`bsd_mitm.cpp:79`, `RegisterMitmServer<BsdMitmService>(PortIndex_BsdU, BsdUName)`). Tailnet destinations (100.64/10, fd7a:115c:a1e0::/48) are served by its own engine. All other calls are forwarded to the real service. No separate client API or IPC exists for apps: the app just uses plain BSD sockets, so a client needs no tailscale-nx-specific code.

### F2. Who gets MITM'd (`BsdMitmService::ShouldMitm`, `bsd_mitm.cpp:469-475`)
In order:
1. Program `0x420000000000000E` (sys-ftpd) is served only if `mitm=...sys-ftpd` is set (special case, and only through the extra `bsd:s` MITM, see F4).
2. Served if `override_status.IsHbl()` (launched via hbloader) or the program ID is in `mitm_titles`.
3. Otherwise served only if the ID is not `01xxxxxxxxxxxxxx` (Nintendo range), is not sys-ftpd, and `IsRunningApplication(c)` is true.

`IsRunningApplication` (`bsd_mitm.cpp:133-146`) asks `pm:dmnt` for the current application PID and compares it to the client PID, with a 50 ms timeout (late answer means "no"). A background sysmodule is not the running application, so rule 3 rejects it.

### F3. `mitm_titles` semantics
- Parsed in `bsd_mitm.cpp:928` (`ParseTitles`, 16 hex digits, `0x` optional, comma or space separated). Max 16 entries (`MaxTitles`), config buffer 384 bytes.
- Stored in `g_titles`, checked by `IsListedTitle` (`bsd_mitm.cpp:90-92`) at `bsd_mitm.cpp:473`.
- The check is a plain program-ID match. There is no type check, so it is not limited to forwarders even though the docs describe it that way. A sysmodule ID listed there would match.
- It is read at boot (restart needed after editing). Docs frame it as "forwarders whose ID starts with 01"; use for a sysmodule is undocumented but follows from the code. This is the only route that needs no tailscale-nx change.
- No exclusion list exists. Exclusion is implicit: Nintendo `01` IDs, sys-ftpd, and non-running-application processes are rejected.

### F4. Which service the client must open
- MITM is on `bsd:u` for everyone except sys-ftpd. sys-ftpd gets a dedicated `bsd:s` MITM (`bsd_mitm.cpp:793-805`, `BsdSystemMitmService::ShouldMitm` returns true only for sys-ftpd), run on its own threads because every system service shares `bsd:s`.
- libnx default is `BsdServiceType_User` (`socket.c:76`), i.e. `bsd:u`. `BsdServiceType_Auto` tries `bsd:s` first, which would bypass the `bsd:u` MITM (`bsd.c:220-228`).
- So the uploader must call `socketInitialize` with `bsd_service_type = BsdServiceType_User` (default) and its NPDM must allow `bsd:u` in its service access list. Check the allowed-services list; this has not been verified on hardware.

### F5. Socket API and IPC options
- Plain BSD sockets (`socket`, `connect`, `send`, `recv`, `poll`/`select` are all handled in `BsdMitmService`). TCP client to a tailnet IP works. The `mitm-test` NRO (`switch/mitm-test/source/main.c`) proves the pattern for homebrew.
- No IPC port, no libnx wrapper and no tailscale-nx control API is needed for sockets. `native/ctl/tsnx_ctl.h` is a status API for the overlay and is not required.
- Per-session cost: ~4.4 KB each, 24 sessions max (`MaxSessions`). libnx opens a monitor session plus a pool of `num_bsd_sessions`. Use a small pool (1 to 2) for the uploader.
- Memory for tailnet sockets is shared (`overlay_budget_kb`, default 320 KB, TCP buffers shrink as it fills).
- Clone failures no longer abort the MITM process thanks to the patch, but a failed session reaches the client as an error. The uploader must tolerate `socketInitialize` and `connect` failures.

### F6. Boot ordering and lifecycle
- The MITM is registered when tailscale-nx starts. Sessions opened by a client before that bypass it. sys-ftpd needs a forced restart for this reason (`RestartSysFtpd`, `bsd_mitm.cpp:895-906`). A custom sysmodule would need either to start after tailscale-nx or to retry and reopen its `bsd:u` sessions on failure or when the tailnet is not yet up (not verified).
- Stopping tailscale-nx is refused while homebrew with live sessions exists (`PrepareStop` returns `Homebrew`/`Busy`, `main.cpp:879-885`), and sleep/wake has its own refusal window. An always-on uploader session would likely block "Stop" in the overlay. Untested.
- Tailnet readiness is separate from the MITM: before the node joins, a connect to 100.x will fail. Retry with backoff.

## Answer: can a background custom sysmodule use the tailnet without modifying tailscale-nx?

Probably yes, by configuration only, with no source change:

1. Give the uploader a stable program ID outside `01...` (and not `0x420000000000000E`).
2. Add that ID to `mitm_titles` in `sdmc:/config/tailscale-nx/config.ini`.
3. Keep `mitm=homebrew` (default). No `sys-ftpd` setting is needed.
4. The uploader opens `bsd:u` only and connects to the tailnet IP.

Evidence is code reading only (F2, F3, F4). It has not been run. The uncertainty is (a) whether a sysmodule's `bsd:u` session query reaches the MITM with the expected `program_id`, (b) NPDM `bsd:u` access, (c) boot ordering (F6).

Without `mitm_titles`, background traffic is not allowed: rule 3 depends on being the running application.

## Homebrew NRO vs background sysmodule as first smoke test

Recommend the NRO first.
- NROs launched from hbmenu get `IsHbl()` true, so they are served with default config. No config change, no NPDM, no boot ordering. The upstream `mitm-test` already shows the pattern.
- It isolates three questions in order: tailnet reachability, HTTP request shape and auth, then data format. The sysmodule question (F2 rule 2, F4, F6) is then a smaller second step.
- It cannot answer: whether `mitm_titles` works for a sysmodule, memory or sleep/wake behavior, or whether the overlay Stop is blocked.
- Smallest NRO: `socketInitializeDefault`, resolve nothing (use the literal IP), POST a few bytes, print the status via `nxlink -s`, `socketExit`.

## Minimal choices

- C1. NRO first (manual button press). Sysmodule only if NRO succeeds.
- C2. Literal tailnet IP, no MagicDNS. Avoids DNS paths.
- C3. Plain HTTP over the tailnet (WireGuard already encrypts it). No TLS stack needed in the add-on.
- C4. Watcher output stays local files. The uploader reads those files and sends them. Watcher has no networking.
- C5. One connection per batch, small payload, retry with backoff. No persistent connection (avoids blocking tailscale-nx Stop).
- C6. Do not use `mitm=homebrew,sys-ftpd`. It adds ~70 KB, restarts sys-ftpd, and uses extra system sessions that can starve other homebrew.

## Test design (Switch to Gerry, tailnet only)

Target: Gerry at `100.116.62.67`. No public ports, no ActivityWatch exposure, no router changes.

Receiver (on Gerry, not part of this repo change):
- A tiny HTTP receiver that accepts a POST and logs size and a hash. It must bind only to Gerry's tailnet address `100.116.62.67`, never `0.0.0.0`. Use an unused high port.
- Optionally restrict by tailnet ACL to the Switch's node only.
- It must not forward to ActivityWatch. ActivityWatch stays on localhost, untouched.

Steps (each reports pass or fail):
1. T1. Gerry reachability: from another tailnet device, `curl http://100.116.62.67:<port>/` works. From a non-tailnet network the port is closed.
2. T2. Switch joined: tailscale-nx overlay shows an IP; Gerry sees the Switch as a peer.
3. T3. NRO via hbmenu: POST a fixed 64-byte payload. Expect the receiver log line with size and hash, and a 2xx on the console via nxlink.
4. T4. Failure modes: tailscale-nx paused, Gerry receiver stopped, wrong port. The NRO must report errors cleanly and exit, with no hang.
5. T5. Larger payload (about 100 KB) to cover buffer behavior under the 320 KB budget.
6. T6. Only after T1-T5: minimal sysmodule that POSTs once at boot plus a retry loop, with its ID in `mitm_titles`. Check the tailscale-nx log for `session from program <ID>`. That log line is the direct evidence for F2/F3.
7. T7. Sysmodule negative control: same sysmodule with its ID removed from `mitm_titles`. Expected: connect to 100.x fails (no route), proving the whitelist is what allows it.
8. T8. Sleep/wake and overlay Stop with the sysmodule running (F6).

No credentials, tokens or raw activity events belong in the test payloads. Use synthetic data.

## Known untested points

- U1. Whether a sysmodule with a listed ID gets MITM'd (code says yes, never run).
- U2. NPDM service access for `bsd:u` and the sysmodule's available socket memory and thread budget.
- U3. Boot ordering: the uploader starting before tailscale-nx, and whether its sessions can be re-established.
- U4. `IsRunningApplication` timeout behavior for non-listed sysmodules (expected to answer "no").
- U5. Overlay Stop behavior with a permanent client session.
- U6. Sleep/wake with a live uploader session.
- U7. Compatibility beyond upstream's tested setup: Atmosphère 1.12.0, firmware 22.5.0, one Mariko console.
- U8. Behavior when the 24-session cap is hit by other homebrew.

## Recommendation

1. Build the NRO smoke test first. It works with a default tailscale-nx install.
2. Run the sysmodule only after the NRO passes, and use `mitm_titles` with the sysmodule's program ID. This needs no change to tailscale-nx, but is untested and relies on undocumented use of `mitm_titles`.
3. Keep the uploader a separate add-on that reads the watcher's local files. The watcher stays unchanged and offline.

## First probe preparation (2026-10-27)

The throwaway NRO source is isolated on branch `prototype/tailnet-sync` at commit `62aa0b3`. It explicitly opens `bsd:u`, sends only a fixed synthetic JSON message on button A, and reads its endpoint and bearer token from a private SD configuration file. Credentials are not committed.

A temporary Python receiver on Gerry was bound only to `100.116.62.67:18765` with an automatic 20-minute service lifetime. Mac-to-receiver checks returned 401 for an invalid token, 200 for the authenticated synthetic message, and 400 for arbitrary data. Socket inspection confirmed the listener was bound to the tailnet IPv4 address, not a wildcard. This does not prove non-tailnet firewall behavior or Switch connectivity.

The NRO and its configuration were copied to the Switch with readback verification. Switch-to-receiver execution, background sysmodule whitelisting, retries, larger payloads, and sleep/wake networking remain untested. The watcher implementation is unchanged. The temporary app, credential file, receiver service, and receiver files should be removed after the experiment.
