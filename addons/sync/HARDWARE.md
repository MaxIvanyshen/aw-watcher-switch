# Hardware validation

Tested on one Switch Lite with firmware 23.0.1, Atmosphere 1.12.1/Kefir 927 and tailscale-nx 1.1.3. These observations do not establish compatibility with other setups.

## Synthetic delivery

The uploader was configured to read an isolated synthetic journal, not the watcher's real journal. Its receiver used a separate SQLite test database.

- After reboot, all five fixture records reached the receiver without opening any homebrew app.
- The uploader reported offset 1073, five acknowledged records, no skips and no error.
- Concatenated stored record bytes matched the fixture's SHA-256 exactly.

## Receiver outage and sleep/wake

The receiver was stopped and a sixth synthetic record was added to the test journal. The user waited 45 seconds, slept the Switch for 30 seconds, woke it and opened MTP.

Before restoring the receiver:

- The database still had five records.
- Uploader status was `retry`, errno 111, offset 1073 and five acknowledged records.
- No record was skipped and the pending cursor had not advanced.

After restoring the receiver:

- A batch from the Switch's tailnet address inserted one new record and no duplicates.
- The uploader reported six acknowledged records, offset 1298, no skipped records and errno zero.
- All six stored records matched the updated synthetic journal byte-for-byte.

This validates eventual delivery across that receiver outage and sleep/wake cycle. It does not prove behavior for every sleep timing or network failure.

## Real-journal activation

After explicit user approval, the real journal was backed up privately and the receiver was switched to a separate production SQLite database. An empty pre-upload database backup was created first. The uploader then read the real journal successfully while the watcher remained enabled, delivered multiple batches, and reported all available records acknowledged with no skipped records or errors. Stored bytes matched the private pre-upload journal backup exactly. No counts, record payloads, credentials or raw diagnostics from real history are committed here.

The retired foreground probe and its configuration, and the isolated synthetic journal/checkpoints, were removed from the SD card. The real journal, uploader configuration and uploader binary were verified to remain.

## Remaining gates

Stress-testing reads exactly while the live watcher appends, reboot persistence against the real journal, and larger payload behavior still need hardware verification. Host tests cover checkpoint-loss replay, lost acknowledgements, conflicts and offline retry. The production receiver keeps the synthetic database separate from real history.
