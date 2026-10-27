# aw-sync: Switch sysmodule

Uploads the watcher's `sessions.jsonl` to the receiver over `bsd:u` (tailscale-nx). Title ID `0x4200000000415755`, services `fsp-srv`, `set:sys`, `bsd:u` only. Hardware behaviour is not verified yet (PLAN section 10).

## Build

```sh
sh addons/sync/switch/build.sh
```

Needs Docker. Uses the pinned `devkitpro/devkita64` image, fails on any warning, runs `check-perms.sh`, prints size and sha256. Output: `dist/addon-sync/sdcard/atmosphere/contents/4200000000415755/{exefs.nsp,flags/boot2.flag}`.

## Install

1. Copy `dist/addon-sync/sdcard/atmosphere` onto the SD card root.
2. Confirm `4200000000415755` is in tailscale-nx `mitm_titles`.
3. Create `/config/aw-watcher-switch-sync/config.txt` (LF only, at most 512 bytes):

```
server=100.64.0.1:18766
device=00000000-0000-0000-0000-000000000000
token=<token, 16 to 128 chars of A-Za-z0-9._~+->
```

   Optional keys: `journal=`, `state_dir=`. The file is re-read every run, so a fix needs no reboot. A changed file restarts the engine (retry backoff resets, the checkpoint stays). The platform only allows writes under paths starting `/config/aw-watcher-switch-sync` (includes `-sync-test`).
4. Reboot.

## Files

`/config/aw-watcher-switch-sync/status.json` shows the last result. `skipped.log` lists skipped journal lines. `sync.a` and `sync.b` are the checkpoint slots.

## Disable and recovery

Delete `flags/boot2.flag` or the title directory to disable. If the console misbehaves at boot, remove the ID from `mitm_titles`. Do not remove the SD card while the console is on.
