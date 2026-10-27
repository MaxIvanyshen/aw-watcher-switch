# sync-probe (throwaway)

Smoke test: can an hbmenu NRO POST synthetic JSON through tailscale-nx to a tailnet-only authenticated endpoint?
It verifies only the homebrew NRO route (bsd:u). It does NOT prove a background sysmodule can do it.
It reads no session data and the repo holds no credentials.

## Build
`prototype/sync-probe/build.sh` (pinned devkitPro image, same as `nx/build.sh`). Output: `dist/sync-probe.nro` (git-ignored).

## Config
`sdmc:/config/aw-watcher-switch-sync-probe/config.txt`, three lines:
```
100.x.y.z      (IPv4)
8080           (port 1-65535)
<token>        (1-128 printable ASCII, no spaces; never displayed)
```

## Run
1. Copy `sync-probe.nro` to `sdmc:/switch/` and create the config file.
2. Launch from hbmenu (applet takeover or title takeover, tailscale-nx running).
3. Press A to send, PLUS to exit. Screen shows connect/send/recv errno or the first response line and PASS (HTTP 200) / FAIL.

## Server contract
```
POST /probe HTTP/1.1
Authorization: Bearer <token>
Content-Type: application/json
Content-Length: 37
Connection: close

{"probe":"aw-watcher-switch","v":1}
```
Expected: `HTTP/1.1 200`. Every step is bounded at 5 s (nonblocking + poll). Sockets use `BsdServiceType_User` explicitly.

## Cleanup
Delete `sdmc:/switch/sync-probe.nro`, `sdmc:/config/aw-watcher-switch-sync-probe/`, host `dist/sync-probe.*`, and rotate/remove the probe token on the server.
