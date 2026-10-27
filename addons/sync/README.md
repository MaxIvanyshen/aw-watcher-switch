# Optional session sync add-on

A separate Switch sysmodule reads the watcher's local JSONL journal and sends completed records to an authenticated, tailnet-only Python/SQLite receiver. The watcher itself remains offline and unchanged. No ActivityWatch integration is included.

## Build and test

```sh
make -C addons/sync test
python3 -m unittest discover -s addons/sync/server -v
python3 addons/sync/e2e.py
sh addons/sync/switch/build.sh
```

The end-to-end test uses synthetic records, temporary files, and loopback only. It exercises delivery, checkpoint-loss replay, lost acknowledgement, conflicting records, and receiver unavailability.

- [Switch configuration and installation](switch/README.md)
- [Receiver configuration and deployment](server/README.md)
- [Wire protocol and recovery design](PLAN.md)
- [Local source format](../../docs/FORMAT.md)

## Delivery behavior

Acknowledgements are bound to the batch ID, SHA-256, and record count. The client advances its checkpoint only after verifying the acknowledgement and rechecking its source journal. A lost checkpoint or acknowledgement causes a replay, not deletion. The receiver deduplicates by device and record key; different bytes under an existing key cause a conflict instead of an overwrite.

The client never changes the watcher journal. It preserves uncertainty labels and durations verbatim. Token and device configuration belong outside Git. Plain HTTP is limited to literal tailnet IPv4 addresses and relies on Tailscale's encrypted transport.

Journal replacement detection compares the first and last 64 bytes behind the cursor. An arbitrary edit that preserves both ranges can evade detection. This is an append-only journal consumer, not a filesystem integrity monitor.

Host checks do not establish live FAT file-sharing compatibility, sleep/wake networking, or firmware startup behavior. Those require a Switch test before enabling real-history uploads.
