#!/bin/sh
# usage: check-perms.sh sync.json aw-sync.elf aw-sync.nsp
# Fails unless: services are exactly fsp-srv/set:sys/bsd:u, no service_host, JSON SVCs == SVC stubs linked in the ELF
# == SVCs in the built NPDM (decoded with hactool), and no nifm/dns/applet/hid/time/psc init symbols are linked.
set -eu
json=$1 elf=$2 nsp=$3
NM=${NM:-aarch64-none-elf-nm}
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
fail() { echo "check-perms: FAIL $*" >&2; exit 1; }

python3 - "$json" "$tmp" <<'PY' || exit 1
import json, sys
d = json.load(open(sys.argv[1]))
if sorted(d['service_access']) != ['bsd:u', 'fsp-srv', 'set:sys'] or d['service_host']:
    sys.exit('check-perms: FAIL services %r host %r' % (d['service_access'], d['service_host']))
if d['title_id'] != '0x4200000000415755':
    sys.exit('check-perms: FAIL title_id')
for c in d['kernel_capabilities']:
    if c['type'] == 'syscalls':
        open(sys.argv[2] + '/json', 'w').write('\n'.join(sorted(c['value'])) + '\n')
    if c['type'] == 'debug_flags' and any(c['value'].values()):
        sys.exit('check-perms: FAIL debug flags set')
PY

"$NM" "$elf" | grep -o 'svc[A-Z][A-Za-z]*' | sort -u > "$tmp/elf"
diff "$tmp/json" "$tmp/elf" >/dev/null || { diff "$tmp/json" "$tmp/elf" >&2 || true; fail "JSON SVCs differ from linked SVC stubs"; }

python3 - "$nsp" "$tmp" <<'PY' || exit 1
import struct, sys
raw = open(sys.argv[1], 'rb').read()
n, strs = struct.unpack_from('<II', raw, 4)
base = 0x10 + n * 0x18 + strs
for i in range(n):
    off, size, no, _ = struct.unpack_from('<QQII', raw, 0x10 + i * 0x18)
    if raw[0x10 + n * 0x18 + no:].split(b'\0')[0] == b'main.npdm':
        open(sys.argv[2] + '/main.npdm', 'wb').write(raw[base + off:base + off + size])
        sys.exit(0)
sys.exit('check-perms: FAIL no main.npdm in nsp')
PY
hactool -t npdm "$tmp/main.npdm" > "$tmp/dec" 2>/dev/null || fail "hactool cannot decode NPDM"
sed -n '/Allowed SVCs:/,$p' "$tmp/dec" | sed 's/.*Allowed SVCs: *//' | grep -o 'svc[A-Za-z]*' | sort -u > "$tmp/npdm"
diff "$tmp/json" "$tmp/npdm" >/dev/null || { diff "$tmp/json" "$tmp/npdm" >&2 || true; fail "NPDM SVCs differ from JSON"; }

bad=$("$NM" "$elf" | awk '{print $NF}' | grep -E '^(nifm|sfdnsres|getaddrinfo|gethostby|time(Initialize|Get)|appletInitialize|hidInitialize|padInitialize|psc|ssl.*Initialize)' || true)
[ -z "$bad" ] || { echo "$bad" >&2; fail "unwanted service init symbols linked"; }
"$NM" "$elf" | grep -q ' T socketInitialize' || fail "socketInitialize not linked"
grep -q 'BsdServiceType_User' source/platform.c || fail "bsd_service_type not explicitly BsdServiceType_User"
if grep -nE 'write_at *\([^;]*(journal|SYNC_PATH_JOURNAL)' ../src/*.c; then fail "write_at called with a journal path"; fi
echo "check-perms: OK ($(wc -l < "$tmp/json" | tr -d ' ') SVCs; services fsp-srv set:sys bsd:u; no nifm/dns/applet/hid/time/psc init)"
