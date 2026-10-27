# sync-probe crash analysis (status: root cause NOT yet determined)

Logs: `/tmp/switch-watcher-first-capture/01793131{131,144,220}_03db12780bd84000.log`.
Deployed NRO `aw-sync-probe.nro` is byte-identical to `dist/sync-probe.nro` (sha256 `dee37c10...1b7f`).

## Facts
- F1. All three logs are the same crash: `User Break`, reason `0x0` (Panic), PC `hbl+0x3a28` (svcBreak), LR `hbl+0x367c`, caller `hbl+0x64c`, break size 4. Only module listed is `hbl` (`255C4EFF9DBA9D5E370C3CBBA206C163334DFED3`). Only ASLR bases differ. There is no first-vs-later difference.
- F2. The abort is in hbloader code, not in the probe. A probe-side libnx abort would show PC in the NRO, not in `hbl`.
- F3. The stored result is 0, not 0x38080. Break Address is `...72c`. In the stack dump `...72c` is `00 00 00 00` (Atmosphere agrees: `Result: 0x0`). `0x38080` sits in the adjacent u32 `...728`, which is unused padding in the `diagAbortWithResult` frame (it stores `res` at `sp+0x1c`, frame `sp..sp+0x20`), so it is stale stack data, not the result.
- F4. 0x38080 is probably not a result code anyway: as a result it would be module 128, description 448, which is not a hbloader code (hbloader is module 347, 0x...15B).
- F5. Stock hbloader v2.4.5 (built from `switchbrew/nx-hbloader` tag, objdump of every `bl diagAbortWithResult`) never aborts with 0. Every site loads a `0x...5b` constant (module 347, desc 1..39, 404) or a failed `rc`. Abort 0 is therefore not reachable in stock hbl source.
- F6 (superseded by F10/F11). The hbl on the console is not stock. Its build id `255C4EFF...` matches none of the hbl.nsp files in the Atmosphere releases 0.9.2 (hbl 2.1) through 1.12.1 (hbl 2.4.5) (checked 11 versions). So hbl+offset cannot be symbolized from upstream.
- F7. Return addresses 01..05 are stale frames of the previous NRO left on the shared hbl/NRO stack (loader resets sp to `__stack_top` before `blr` into the NRO). They span 0x28ac00 bytes, but the probe is 0x38000 bytes (text 0x2a000), so they are not probe code. A 2.4 MB hbmenu-sized NRO ran before. Conclusion: hbmenu ran, requested the probe, returned to hbl, hbl aborted while handling the next load (or just after the probe exited).
- F8. NRO header of the probe is normal: size 0x38000, text 0x0/0x2a000, rodata 0x2a000/0x9000, data 0x33000/0x5000, bss 0x4000. hbl's own checks (magic, segment bounds) pass on it. No `-Wl` or Makefile flag is unusual (`-mtp=soft -fPIE`, `switch.specs`, devkitA64 r29).
- F9. Result does NOT map to an upstream hbl enum, so I cannot name the failing check from the log alone.

- F10. Console `sdmc:/atmosphere/hbl.nsp` (42209 B) is byte-identical to the stock Atmosphere 1.9.5 `hbl.nsp` (hbl 2.4.4, NSO build id `A68126CB...`). So F6 was wrong about "non-stock", but the crash log's module id `255C4EFF...` does not match this file.
- F11. I decompressed the 2.4.4 text (0xa968 bytes) and disassembled it. At `+0x64c` there is no call return (preceded by `adrp`), and `+0x3a28` is an address-field `ubfx`, not an `svc`. So the crashing "hbl" module is not this NSO's code, and the abort cannot be mapped into it.
- F12. Against the probe ELF (`dist/sync-probe.elf`), `+0x64c` is exactly the return address after `bl connect` in `probe()` (main.c:70), but `+0x3a28`/`+0x367c` are `envSetup`/`serviceCreate`, not an `svc`/connect path. So the log offsets do not map consistently onto the probe either. The "hbl" module in the log is 0x6000 bytes, smaller than hbl text (0xa968) and the probe text (0x2a000), so the label and base are unreliable. Mapping offset to code is not proven for either binary.

## Not established
Which code actually aborted, and whether it is before the probe screen or at connect(). `X6 = "NRO0"` and stale `NRO0` in the stack show an NRO was being loaded or entered.

## Minimal test (A1)
Run `dist/hello-probe.nro` (143360 B, sha256 `d49efa62...0177`, no bsd) from the same launch mode.
- Crashes: launch mode or hbl/Atmosphere setup, not the network code.
- Works: run `sync-probe.nro` again and note whether "aw sync probe" and `socketInitialize rc` appear (rc visible means startup is fine and the abort is in bsd or connect).

## Fix
None. Evidence does not identify a probe defect.
