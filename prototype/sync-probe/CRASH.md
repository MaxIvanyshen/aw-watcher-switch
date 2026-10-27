# sync-probe crash analysis (status: root cause NOT yet determined)

Logs: `/tmp/switch-watcher-first-capture/01793131{131,144,220}_03db12780bd84000.log`.
Deployed NRO `aw-sync-probe.nro` is byte-identical to `dist/sync-probe.nro` (sha256 `dee37c10...1b7f`).

## Facts
- F1. All three logs are the same crash: `User Break`, reason `0x0` (Panic), PC `hbl+0x3a28` (svcBreak), LR `hbl+0x367c`, caller `hbl+0x64c`, break size 4. Only module listed is `hbl` (`255C4EFF9DBA9D5E370C3CBBA206C163334DFED3`). Only ASLR bases differ. There is no first-vs-later difference.
- F2. The abort is in hbloader code, not in the probe. A probe-side libnx abort would show PC in the NRO, not in `hbl`.
- F3. The stored result is 0, not 0x38080. Break Address is `...72c`. In the stack dump `...72c` is `00 00 00 00` (Atmosphere agrees: `Result: 0x0`). `0x38080` sits in the adjacent u32 `...728`, which is unused padding in the `diagAbortWithResult` frame (it stores `res` at `sp+0x1c`, frame `sp..sp+0x20`), so it is stale stack data, not the result.
- F4. 0x38080 is probably not a result code anyway: as a result it would be module 128, description 448, which is not a hbloader code (hbloader is module 347, 0x...15B).
- F5. Stock hbloader v2.4.5 (built from `switchbrew/nx-hbloader` tag, objdump of every `bl diagAbortWithResult`) never aborts with 0. Every site loads a `0x...5b` constant (module 347, desc 1..39, 404) or a failed `rc`. Abort 0 is therefore not reachable in stock hbl source.
- F6. The hbl on the console is not stock. Its build id `255C4EFF...` matches none of the hbl.nsp files in the Atmosphere releases 0.9.2 (hbl 2.1) through 1.12.1 (hbl 2.4.5) (checked 11 versions). So hbl+offset cannot be symbolized from upstream.
- F7. Return addresses 01..05 are stale frames of the previous NRO left on the shared hbl/NRO stack (loader resets sp to `__stack_top` before `blr` into the NRO). They span 0x28ac00 bytes, but the probe is 0x38000 bytes (text 0x2a000), so they are not probe code. A 2.4 MB hbmenu-sized NRO ran before. Conclusion: hbmenu ran, requested the probe, returned to hbl, hbl aborted while handling the next load (or just after the probe exited).
- F8. NRO header of the probe is normal: size 0x38000, text 0x0/0x2a000, rodata 0x2a000/0x9000, data 0x33000/0x5000, bss 0x4000. hbl's own checks (magic, segment bounds) pass on it. No `-Wl` or Makefile flag is unusual (`-mtp=soft -fPIE`, `switch.specs`, devkitA64 r29).
- F9. Result does NOT map to an upstream hbl enum, so I cannot name the failing check from the log alone.

## Not established
- Which hbl instruction at `hbl+0x64c` leads to abort(0), and whether it happens before the probe starts or after the probe returns.

## Needed from main (no SD changes)
1. `sdmc:/atmosphere/hbl.nsp` from the console (and `atmosphere/package3` version/`hbmenu.nro` size) so `hbl+0x64c`/`+0x3a28` can be disassembled against build id 255C4EFF.
2. Did the probe console screen ("aw sync probe") ever appear before the crash? Launch mode used (title takeover of which game vs applet/album)?
3. Control: does a stock libnx hello-world NRO launched the same way work? That separates "probe-specific" from "hbl/launch-mode-specific".

## Fix
None applied. No evidence yet that the probe NRO is at fault, and changing it blindly is unjustified.
