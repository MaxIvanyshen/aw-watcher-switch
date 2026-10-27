#!/usr/bin/env python3
"""aw-switch sync receiver: plain HTTP on the tailnet, SQLite, one transaction per batch (PLAN sections 3-4)."""
import argparse, hashlib, hmac, ipaddress, json, os, re, sqlite3, sys, threading, time, zlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

MAX_BODY, MAX_RECORDS, MAX_LINE, MAX_CONNS = 65536, 256, 384, 16
SOCKET_TIMEOUT, BODY_DEADLINE, BUSY_S = 10, 30, 5
TAILNET = ipaddress.ip_network("100.64.0.0/10")
UUID_RE = re.compile(r"[0-9a-f]{8}(-[0-9a-f]{4}){3}-[0-9a-f]{12}")
TOKEN_RE = re.compile(rb"[A-Za-z0-9._~+-]{16,128}")
BID_RE = re.compile(r"[0-9a-f]{16}-[0-9]{1,20}-([0-9]{1,3})")
SHA_RE = re.compile(r"[0-9a-f]{64}")
DIGITS = re.compile(r"[0-9]{1,9}")
SUFFIX = re.compile(rb',"c":"([0-9a-f]{8})"\}\n$')
SCHEMA = """
CREATE TABLE IF NOT EXISTS batches(device TEXT, batch_id TEXT, sha256 TEXT, count INT, new INT, dup INT,
  received_at INT, PRIMARY KEY(device, batch_id)) WITHOUT ROWID;
CREATE TABLE IF NOT EXISTS records(device TEXT, ep INT, ev INT, sub INT, line BLOB NOT NULL, batch_id TEXT,
  received_at INT, PRIMARY KEY(device, ep, ev, sub)) WITHOUT ROWID;
"""


def load_config(path, allow_loopback=False):
    with open(path) as f:
        c = json.load(f)
    if not isinstance(c, dict) or set(c) != {"bind", "port", "db", "token_file", "devices"}:
        raise ValueError("config keys must be exactly bind, port, db, token_file, devices")
    if not isinstance(c["bind"], str):
        raise ValueError("bind must be an IPv4 string")
    ip = ipaddress.IPv4Address(c["bind"])
    if not (ip in TAILNET or (allow_loopback and ip.is_loopback)):
        raise ValueError("bind must be inside 100.64.0.0/10 (loopback needs --allow-loopback)")
    port = c["port"]
    if type(port) is not int or not (0 < port < 65536 or (port == 0 and allow_loopback)):
        raise ValueError("bad port")
    devs = c["devices"]
    if not isinstance(devs, list) or not devs or not all(isinstance(d, str) and UUID_RE.fullmatch(d) for d in devs):
        raise ValueError("devices must be a non-empty list of lowercase UUIDs")
    if not isinstance(c["db"], str) or not isinstance(c["token_file"], str):
        raise ValueError("db and token_file must be strings")
    return {"bind": c["bind"], "port": port, "db": os.path.expanduser(c["db"]), "devices": set(devs),
            "token": read_token(os.path.expanduser(c["token_file"]))}


def read_token(path):
    with open(path, "rb") as f:
        st = os.fstat(f.fileno())
        if st.st_mode & 0o077 or st.st_uid != os.geteuid():
            raise ValueError("token file must be owned by this user with mode 600")
        tok = f.read(512).strip()
    if not TOKEN_RE.fullmatch(tok):
        raise ValueError("token must match [A-Za-z0-9._~+-]{16,128}")
    return tok


def connect(path):
    db = sqlite3.connect(path, timeout=BUSY_S, isolation_level=None)
    db.execute("PRAGMA synchronous=FULL")
    db.execute("PRAGMA foreign_keys=ON")
    return db


def init_db(path):
    os.makedirs(os.path.dirname(path) or ".", mode=0o700, exist_ok=True)
    db = connect(path)
    db.execute("PRAGMA journal_mode=WAL")
    db.executescript(SCHEMA)
    db.close()


class Bad(ValueError):
    pass


def _no_dups(pairs):
    d = {}
    for k, v in pairs:
        if k in d:
            raise Bad("duplicate key")
        d[k] = v
    return d


def _no_float(_):
    raise Bad("float or constant")


def _isint(x, lo, hi):
    return type(x) is int and lo <= x <= hi


def parse_line(line):
    """line includes its newline. Returns the (ep, ev, sub) key or raises Bad."""
    if len(line) > MAX_LINE or not line.startswith(b'{"v":1,'):
        raise Bad("shape")
    m = SUFFIX.search(line)
    if not m or zlib.crc32(line[:m.start()]) != int(m[1], 16):
        raise Bad("crc")
    try:
        r = json.loads(line, object_pairs_hook=_no_dups, parse_float=_no_float, parse_constant=_no_float)
    except Exception:
        raise Bad("json")
    if not isinstance(r, dict):
        raise Bad("json")
    t, ep, ev, sub = r.get("t"), r.get("ep"), r.get("ev"), r.get("sub")
    if not (_isint(r.get("v"), 1, 1) and t in ("session", "gap") and _isint(ep, 1, 2**32 - 1)
            and _isint(ev, 0, 2**31 - 1) and _isint(sub, 0, 1) and sub == (0 if t == "gap" else 1)
            and r.get("c") == m[1].decode()):
        raise Bad("fields")
    return ep, ev, sub


def ack_bytes(bid, sha, cnt, new, dup):
    return ('{"v":1,"id":"%s","sha256":"%s","count":%d,"new":%d,"dup":%d}' % (bid, sha, cnt, new, dup)).encode()


def store(path, dev, bid, sha, recs):
    """One atomic batch. Returns ('ok', new, dup) or ('conflict', key)."""
    db = connect(path)
    try:
        db.execute("BEGIN IMMEDIATE")
        row = db.execute("SELECT sha256,count,new,dup FROM batches WHERE device=? AND batch_id=?", (dev, bid)).fetchone()
        if row:
            db.execute("ROLLBACK")
            return ("ok", row[2], row[3]) if row[:2] == (sha, len(recs)) else ("conflict", "batch")
        new = dup = 0
        now = int(time.time())
        for key, line in recs:
            old = db.execute("SELECT line FROM records WHERE device=? AND ep=? AND ev=? AND sub=?", (dev, *key)).fetchone()
            if old is None:
                db.execute("INSERT INTO records VALUES(?,?,?,?,?,?,?)", (dev, *key, line, bid, now))
                new += 1
            elif old[0] == line:
                dup += 1
            else:
                db.execute("ROLLBACK")
                return "conflict", "%d/%d/%d" % key
        db.execute("INSERT INTO batches VALUES(?,?,?,?,?,?,?)", (dev, bid, sha, len(recs), new, dup, now))
        db.execute("COMMIT")
        return "ok", new, dup
    finally:
        db.close()


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    server_version, sys_version = "awsync", ""

    def setup(self):
        self.timeout = SOCKET_TIMEOUT
        super().setup()

    def log_message(self, fmt, *args):
        sys.stderr.write("%s %s\n" % (self.client_address[0], fmt % args))

    def send_error(self, code, message=None, explain=None):
        self.close_connection = True
        self.reply(code, '{"v":1,"error":"http"}')

    def reply(self, code, body):
        body = body.encode() if isinstance(body, str) else body
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(body)

    def fail(self, code, name, key=None):
        self.reply(code, json.dumps({"v": 1, "error": name, **({"key": key} if key else {})}, separators=(",", ":")))

    def one(self, name):
        v = self.headers.get_all(name) or []
        return v[0] if len(v) == 1 else None

    def read_body(self, n):
        end, buf = time.monotonic() + BODY_DEADLINE, bytearray()
        while len(buf) < n:
            if time.monotonic() > end:
                raise TimeoutError("body deadline")
            chunk = self.rfile.read1(min(8192, n - len(buf)))
            if not chunk:
                break
            buf += chunk
        return bytes(buf)

    def do_POST(self):
        self.close_connection = True
        if not self.server.slots.acquire(blocking=False):
            return self.fail(503, "busy")
        try:
            self.handle_batch()
        finally:
            self.server.slots.release()

    def handle_batch(self):
        srv = self.server
        if self.path != "/v1/batch":
            return self.fail(404, "not_found")
        cl = self.one("Content-Length")
        if "Transfer-Encoding" in self.headers or cl is None or not DIGITS.fullmatch(cl):
            return self.fail(400, "framing")
        n = int(cl)
        if n > MAX_BODY:
            return self.fail(413, "too_large")
        auth = (self.one("Authorization") or "").encode("utf-8", "replace")
        if not hmac.compare_digest(auth, b"Bearer " + srv.token):
            return self.fail(401, "unauthorized")
        dev = self.one("X-Device")
        if dev not in srv.devices:
            return self.fail(403, "forbidden")
        bid, cnt, sha = self.one("X-Batch-Id"), self.one("X-Batch-Count"), self.one("X-Batch-Sha256")
        m = BID_RE.fullmatch(bid or "")
        if not (m and cnt == m[1] and 1 <= int(cnt) <= MAX_RECORDS and SHA_RE.fullmatch(sha or "")):
            return self.fail(400, "headers")
        body = self.read_body(n)
        if len(body) != n or hashlib.sha256(body).hexdigest() != sha or not body.endswith(b"\n"):
            return self.fail(400, "body")
        lines = [x + b"\n" for x in body.split(b"\n")[:-1]]
        if len(lines) != int(cnt):
            return self.fail(400, "count")
        try:
            recs = [(parse_line(x), x) for x in lines]
        except Bad as e:
            return self.fail(400, "line_" + str(e).split()[0])
        try:
            res = store(srv.db, dev, bid, sha, recs)
        except sqlite3.Error as e:
            self.log_message("db error %s", type(e).__name__)
            return self.fail(503, "busy")
        if res[0] == "conflict":
            self.log_message("conflict dev=%s id=%s key=%s", dev[:8], bid, res[1])
            return self.fail(409, "conflict", res[1])
        self.log_message("batch dev=%s id=%s new=%d dup=%d", dev[:8], bid, res[1], res[2])
        if os.environ.get("AWSYNC_TEST_DROP_RESPONSE"):
            return
        self.reply(200, ack_bytes(bid, sha, int(cnt), res[1], res[2]))


class Server(ThreadingHTTPServer):
    daemon_threads = True
    request_queue_size = 16

    def handle_error(self, request, client_address):
        sys.stderr.write("%s error %s\n" % (client_address[0], sys.exc_info()[0].__name__))


def make_server(cfg):
    init_db(cfg["db"])
    s = Server((cfg["bind"], cfg["port"]), Handler)
    s.token, s.devices, s.db = cfg["token"], cfg["devices"], cfg["db"]
    s.slots = threading.BoundedSemaphore(MAX_CONNS)
    return s


def dump_keys(config_path, dev):
    with open(config_path) as f:
        db = os.path.expanduser(json.load(f)["db"])
    if not os.path.exists(db):
        raise ValueError("no database at " + db)
    for row in connect(db).execute("SELECT ep,ev,sub FROM records WHERE device=? ORDER BY ep,ev,sub", (dev,)):
        print(*row)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--config", default=os.environ.get("AWSYNC_CONFIG", "~/.config/aw-switch-sync/config.json"))
    ap.add_argument("--allow-loopback", action="store_true", help="tests only")
    ap.add_argument("--dump-keys", metavar="DEVICE", help="print 'ep ev sub' for a device and exit")
    a = ap.parse_args(argv)
    os.umask(0o077)
    path = os.path.expanduser(a.config)
    try:
        if a.dump_keys:
            return dump_keys(path, a.dump_keys)
        srv = make_server(load_config(path, a.allow_loopback))
    except (OSError, ValueError, KeyError, TypeError, sqlite3.Error) as e:
        sys.exit("error: %s" % e)
    sys.stderr.write("listening on %s:%d\n" % srv.server_address)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        srv.server_close()


if __name__ == "__main__":
    main()
