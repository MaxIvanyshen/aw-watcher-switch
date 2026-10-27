#!/bin/sh
# Build-time permission check for the watcher NPDM and ELF.
# usage: check-no-net.sh watcher.json aw-watcher.elf [aw-watcher.nsp]
# Fails on any mismatch. Checks:
#   1. service_access is exactly the allowed list and service_host is empty
#   2. the JSON syscall list is exactly nx/allowed-syscalls.txt
#   3. the built NPDM grants exactly those SVCs (decoded from the exefs main.npdm with hactool)
#   4. every svc* stub linked into the ELF is in that list (nothing granted is unused, nothing used is missing)
#   5. no network symbols or network service names are linked
set -eu
json=$1
elf=$2
nsp=${3:-}
here=$(cd "$(dirname "$0")" && pwd)
allow=$here/allowed-syscalls.txt
NM=${NM:-aarch64-none-elf-nm}
STRINGS=${STRINGS:-aarch64-none-elf-strings}
PYTHON=${PYTHON:-python3}
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

fail() { echo "check-no-net: FAIL $*" >&2; exit 1; }

diff_lists() {
  printf '%s\n' "$1" > "$tmp/a"
  printf '%s\n' "$2" > "$tmp/b"
  extra=$(grep -Fxvf "$tmp/b" "$tmp/a" || true)
  missing=$(grep -Fxvf "$tmp/a" "$tmp/b" || true)
  [ -z "$extra" ] && [ -z "$missing" ] && return 0
  [ -z "$extra" ] || echo "$3: present but not allowed: $(echo $extra)" >&2
  [ -z "$missing" ] || echo "$3: allowed but absent: $(echo $missing)" >&2
  return 1
}

# --- 1-3. NPDM json and the built exefs -------------------------------------------------
"$PYTHON" - "$json" "$allow" "$nsp" <<'PY' || exit 1
import json, os, struct, subprocess, sys, tempfile

path, allow_path, nsp = sys.argv[1], sys.argv[2], sys.argv[3]
allowed = sorted(l.strip() for l in open(allow_path) if l.strip() and not l.startswith('#'))
d = json.load(open(path))

def bad(msg):
    print('check-no-net: FAIL ' + msg, file=sys.stderr)
    sys.exit(1)

want_services = ['fsp-srv', 'pdm:qry', 'set:sys', 'time:u']
if sorted(d.get('service_access') or []) != want_services:
    bad('service_access is %r, expected %r' % (d.get('service_access'), want_services))
if d.get('service_host'):
    bad('service_host is not empty: %r' % (d.get('service_host'),))

caps = d.get('kernel_capabilities') or []
svc = [c for c in caps if c.get('type') == 'syscalls']
if len(svc) != 1:
    bad('expected exactly one syscalls capability, found %d' % len(svc))
granted = sorted(svc[0]['value'])
if granted != allowed:
    print('JSON grants but not allowed:', sorted(set(granted) - set(allowed)), file=sys.stderr)
    print('allowed but not granted:', sorted(set(allowed) - set(granted)), file=sys.stderr)
    bad('JSON syscall list differs from nx/allowed-syscalls.txt')

if nsp:
    raw = open(nsp, 'rb').read()
    if raw[:4] != b'PFS0':
        bad('%s is not a PFS0' % nsp)
    n, strings = struct.unpack_from('<II', raw, 4)
    base = 0x10 + n * 0x18 + strings
    npdm = None
    for i in range(n):
        off, size, nameoff, _ = struct.unpack_from('<QQII', raw, 0x10 + i * 0x18)
        name = raw[0x10 + n * 0x18 + nameoff:].split(b'\0')[0]
        if name == b'main.npdm':
            npdm = raw[base + off:base + off + size]
    if npdm is None:
        bad('no main.npdm in the exefs')
    fd, tmpnpdm = tempfile.mkstemp(suffix='.npdm')
    with os.fdopen(fd, 'wb') as f:
        f.write(npdm)
    try:
        tool = os.environ.get('HACTOOL', 'hactool')
        out = subprocess.run([tool, '-t', 'npdm', tmpnpdm], capture_output=True, text=True)
    finally:
        os.unlink(tmpnpdm)
    if out.returncode != 0:
        bad('hactool could not decode the NPDM')
    # hactool prints the first SVC on the "Allowed SVCs:" line and the rest as indented continuations.
    granted, in_svc = [], False
    for line in out.stdout.splitlines():
        if 'Allowed SVCs:' in line:
            in_svc = True
            granted.append(line.split('Allowed SVCs:')[1].split('(')[0].strip())
            continue
        s = line.strip()
        if in_svc and s.startswith('svc') and '(' in s:
            granted.append(s.split('(')[0].strip())
        elif in_svc and s:
            in_svc = False
    built = sorted(set(granted))
    if built != allowed:
        print('NPDM grants but not allowed:', sorted(set(built) - set(allowed)), file=sys.stderr)
        print('allowed but not granted:', sorted(set(allowed) - set(built)), file=sys.stderr)
        bad('NPDM SVC grants differ from nx/allowed-syscalls.txt')
    print('check-no-net: built NPDM grants exactly %d allowed SVCs, no privileged extras' % len(built))

print('check-no-net: services and %d SVC grants OK' % len(allowed))
PY

# --- 4. linked svc stubs --------------------------------------------------------------
linked=$("$NM" "$elf" | grep -o 'svc[A-Z][A-Za-z]*' | sort -u)
allowed=$(grep -v '^#' "$allow" | sed '/^$/d' | sort)
diff_lists "$linked" "$allowed" "linked SVC stubs" || fail "linked SVC stubs differ from nx/allowed-syscalls.txt"

# --- 5. network -----------------------------------------------------------------------
bad=$("$NM" "$elf" | awk '{print $NF}' | grep -E '^(bsd|socket|nifm|curl|ssl|sfdnsres|getaddrinfo|connect$|send(to)?$|recv(from)?$)' || true)
[ -z "$bad" ] || { echo "$bad" >&2; fail "network symbols linked"; }

badstr=$("$STRINGS" -a "$elf" | grep -E '^(bsd:[us]|nifm:[usa]|ssl$|sfdnsres)' || true)
[ -z "$badstr" ] || { echo "$badstr" >&2; fail "network service names in binary"; }

echo "check-no-net: OK ($(echo "$allowed" | wc -l | tr -d ' ') SVCs, no network symbols or service names)"
