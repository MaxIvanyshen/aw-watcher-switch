# Build environment (devkitPro Switch sysmodule, arm64 mac)

## Image
- `devkitpro/devkita64` is multi-arch: `linux/amd64` and `linux/arm64`. No emulation needed on Apple silicon.
- Pulled digest (arm64, what a plain `docker pull` selects here):
  `devkitpro/devkita64@sha256:1fc388c3a0d34bd2045a6dadcb1020e069d5f876a187fd705de14b4440c00282`
- Pin it in commands for reproducibility (the `latest` tag moves).

## Tool versions (inside image)
- aarch64-none-elf-g++ (devkitA64) 15.2.0 (devkitA64 r29.2-1, gcc 15.2.0-7, binutils 2.45.1-2, newlib 4.6.0.20260123-4)
- libnx 4.12.0-1
- switch-tools 1.13.1-1 (npdmtool, elf2nso, build_pfs0, nacptool)
- Docker server 28.3.3, arm64

## Paths
- `DEVKITPRO=/opt/devkitpro` is set in the image. `DEVKITARM` is also set, but irrelevant.
- Compiler: `/opt/devkitpro/devkitA64/bin/aarch64-none-elf-g++`
- Rules: `/opt/devkitpro/libnx/switch_rules` (includes `devkitA64/base_rules`), `switch.specs`, `switch.ld`
- Tools: `/opt/devkitpro/tools/bin/{npdmtool,elf2nso,build_pfs0,nacptool}`
- libnx: `/opt/devkitpro/libnx/{include/switch.h,lib/libnx.a}`

## Tested command
Compiler and tools are NOT on PATH by default. Add them:

```sh
docker run --rm -v "$PWD":/w -w /w \
  devkitpro/devkita64@sha256:1fc388c3a0d34bd2045a6dadcb1020e069d5f876a187fd705de14b4440c00282 \
  bash -c 'export PATH=$DEVKITPRO/devkitA64/bin:$DEVKITPRO/tools/bin:$PATH
           make'
```

Verified with a toy sysmodule (`smInitialize` in `__appInit`, `-specs=$DEVKITPRO/libnx/switch.specs -march=armv8-a+crc+crypto -mtp=soft -fPIE -lnx`):
produced `.elf` (2.3 MB), `elf2nso` -> `.nso`, `npdmtool main.json` -> `.npdm`. Run from /tmp, nothing left in the repo.

## Caveats
- `npdmtool` prints "Failed to get ... (field not present)" for optional fields (system_resource_size, version, program_id*, etc.). Harmless, output is still written.
- Files written by the container are owned by root on the host bind mount. Add `--user "$(id -u):$(id -g)"` if that matters (not tested).
- A Makefile `include $(DEVKITPRO)/libnx/switch_rules` works only with `DEVKITPRO` set (it is in the image).
- No host install, no changes to `server/Switch`.
