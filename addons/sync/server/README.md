# aw-switch sync receiver

Python 3 stdlib only. Protocol and rules: `../PLAN.md` sections 3 and 4.

## Test

    python3 -m unittest discover -s addons/sync/server -v

## Run

    umask 077; mkdir -p ~/.config/aw-switch-sync
    openssl rand -hex 32 > ~/.config/aw-switch-sync/token        # mode 600, 16+ chars
    cp config.example.json ~/.config/aw-switch-sync/config.json  # set bind (own tailnet IP), devices
    python3 awsync_server.py --config ~/.config/aw-switch-sync/config.json

`bind` must be inside 100.64.0.0/10. `--allow-loopback` permits 127.0.0.0/8 and port 0 for tests. `AWSYNC_CONFIG` is the default for `--config`.

## Deploy (user systemd, no root)

    mkdir -p ~/.local/share/aw-switch-sync ~/.config/systemd/user
    cp awsync_server.py ~/.local/share/aw-switch-sync/
    cp awsync.service ~/.config/systemd/user/
    systemctl --user daemon-reload && systemctl --user enable --now awsync
    ss -ltn | grep 18766        # must show only the tailnet address
    journalctl --user -u awsync -f

Needs `loginctl enable-linger` (already on). Logs carry client IP, device prefix, batch id and counts, never the token or headers.

## Query

    sqlite3 ~/.local/share/aw-switch-sync/sync.db 'select count(*) from records'
    python3 awsync_server.py --dump-keys <device-uuid>           # "ep ev sub", key order

Back up before the real-history upload: `sqlite3 sync.db ".backup sync.bak"`.
