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

## Remaining gates

Real-history upload, reading while the live watcher appends, reboot persistence against the real journal and larger payload behavior still need hardware verification. Host tests cover checkpoint-loss replay, lost acknowledgements, conflicts and offline retry. No real-history payloads, credentials or raw diagnostics are committed here.
