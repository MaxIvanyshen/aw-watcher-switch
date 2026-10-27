# background-probe (throwaway)

Finite sysmodule smoke test: can a boot2 sysmodule POST synthetic JSON through tailscale-nx via `mitm_titles`?
Follows the foreground `sync-probe` NRO test. Not a production uploader. Never reads sessions, diagnostics or account logs. No credentials in the repo.

Program ID `0x4200000000415755` (AWSU). `AppletType_None`, 256 KiB static heap, no applet/hid/time/nifm/sfdnsres/psc. Does NOT register `psc:m`.

## Build
`prototype/background-probe/build.sh` (pinned devkitPro image). Output, git-ignored:
`dist/background-probe/sdcard/atmosphere/contents/4200000000415755/{exefs.nsp,flags/boot2.flag}`.
The build runs `check-perms.sh`: NPDM services are exactly `fsp-srv set:sys bsd:u`, JSON SVCs == SVC stubs linked in the ELF == SVCs in the built NPDM, no debug flags, no network-adjacent init symbols.

## NPDM notes
- Services: `fsp-srv` (config/status on sdmc), `set:sys` (firmware version, libnx needs it to pick the bsd ABI), `bsd:u` (tailscale-nx hooks bsd:u only).
- `socketInitialize` (libnx `b156b47`/4.12) needs only `bsd:u` plus a transfer memory (`svcCreateTransferMemory`). It never calls nifm or sfdnsres. libnx still links `nifm.o`/`time.o` code and strings, but nothing initialises them.
- Socket config: `bsd_service_type = BsdServiceType_User`, 1 session, 1 buffer set, fixed 16 KiB TCP buffers, transfer memory 0x1D000.
- Filesystem permissions are `0xffffffffffffffff` (copied from `nx/watcher.json`, not narrowed). Narrowing is future work.
- Pool partition 2 (system), no debug capabilities, thread priority 24-63 on core 3, 18 SVCs.

## Config (same as sync-probe)
`sdmc:/config/aw-watcher-switch-sync-probe/config.txt`: IPv4, port, token (3 lines). Server contract: `POST /probe` with Bearer token and body `{"probe":"aw-watcher-switch","v":1}`, expects HTTP 200.

## Behaviour
At boot: set up sm, fs, firmware version, sdmc with retries. Each attempt: read config, `socketInitialize`, nonblocking connect/send/recv (5 s per step, fcntl/getsockopt checked), `socketExit`. On failure it sleeps 1,2,4,8,10,10... s and reopens the socket service (so a boot-before-tailscale session is replaced). Gives up 180 s after the first attempt. On HTTP 200 it exits cleanly. It does not loop forever.

Status, best effort: `sdmc:/config/aw-watcher-switch-sync-probe/background-status.json`
`{"stage":"done","result":"pass","errno":0,"http":200,"attempts":1}`. `result` is `running|pass|fail|gave_up`. For stage `socket_init` the `errno` field holds the libnx Result.

## Install
1. Close the sync-probe NRO and any other hbmenu app, then power off cleanly if possible.
2. Copy `dist/background-probe/sdcard/atmosphere/` onto the SD card (merge).
3. MERGE the title into `sdmc:/config/tailscale-nx/config.ini` `mitm_titles`, keeping the whole existing tailscale config and existing entries (comma-separated, max 16, 16 hex digits): `mitm_titles=...,4200000000415755`. Do not replace the file.
4. Make sure the config.txt from sync-probe exists. Reboot (boot2.flag starts it). Read `background-status.json`, and the tailscale-nx log line `session from program 4200000000415755`.

## Recovery / disable
Delete `sdmc:/atmosphere/contents/4200000000415755/flags/boot2.flag` (or the whole title directory) and reboot. If boot fails, hold the Atmosphere safe key or remove the directory from the SD on a PC. Remove the ID from `mitm_titles` to undo the whitelist.

## Limitations
Source and build validations only, no hardware run. Unknowns: whether a sysmodule `bsd:u` session reaches the MITM with this program ID, boot ordering versus tailscale-nx, and whether the NPDM permissions suffice on firmware (all unverified).
