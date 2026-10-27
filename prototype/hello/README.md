# hello-probe (control, throwaway)
Console + pad only, no sockets. Same toolchain and flags as `../sync-probe`. Build: `prototype/hello/build.sh` -> `dist/hello-probe.nro` (git-ignored). PLUS exits.
Purpose: if this crashes in the same launch mode, the problem is not the network code.
Cleanup: delete `sdmc:/switch/hello-probe.nro` and `dist/hello-probe.*`.
