import contextlib, hashlib, http.client, io, json, os, socket, sqlite3, stat, tempfile, threading, time, unittest
from unittest import mock
import awsync_server as S

DEV = "00000000-0000-0000-0000-000000000000"
TOKEN = "t" * 16 + "0123456789abcdef"
FIXTURE = os.path.join(os.path.dirname(__file__), "../../../tests/fixtures/synthetic-diag.expected")


def seal(prefix):
    return prefix + b',"c":"%08x"}\n' % S.zlib.crc32(prefix)


def rec(ep=1, ev=1, sub=1, t="session", extra=b'"dur":999999'):
    return seal(b'{"v":1,"t":"%s","ep":%d,"ev":%d,"sub":%d,%s' % (t.encode(), ep, ev, sub, extra))


def batch_headers(body, cnt, off=0, dev=DEV, token=TOKEN, bid=None):
    sha = hashlib.sha256(body).hexdigest()
    bid = bid or "%s-%d-%d" % (sha[:16], off, cnt)
    return bid, sha, {"Authorization": "Bearer " + token, "X-Device": dev, "X-Batch-Id": bid, "X-Batch-Count": str(cnt),
                      "X-Batch-Sha256": sha, "Content-Type": "application/x-ndjson"}


class Base(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.cfg_path = self.write_config()
        self.start()

    def write_config(self, **over):
        tok = os.path.join(self.tmp.name, "token")
        if not os.path.exists(tok):
            with open(tok, "w") as f:
                f.write(TOKEN + "\n")
            os.chmod(tok, 0o600)
        c = {"bind": "127.0.0.1", "port": 0, "db": os.path.join(self.tmp.name, "d", "sync.db"), "token_file": tok, "devices": [DEV]}
        c.update(over)
        p = os.path.join(self.tmp.name, "config.json")
        with open(p, "w") as f:
            json.dump(c, f)
        return p

    def start(self):
        self.cfg = S.load_config(self.cfg_path, allow_loopback=True)
        self.srv = S.make_server(self.cfg)
        self.port = self.srv.server_address[1]
        self.thread = threading.Thread(target=self.srv.serve_forever, kwargs={"poll_interval": 0.01}, daemon=True)
        self.thread.start()
        self.addCleanup(self.stop)

    def stop(self):
        if self.thread.is_alive():
            self.srv.shutdown()
            self.srv.server_close()
            self.thread.join()

    def post(self, body, cnt=None, hdrs=None, **kw):
        cnt = body.count(b"\n") if cnt is None else cnt
        bid, sha, h = batch_headers(body, cnt, **kw)
        h.update(hdrs or {})
        c = http.client.HTTPConnection("127.0.0.1", self.port, timeout=10)
        c.request("POST", "/v1/batch", body, h)
        r = c.getresponse()
        data = r.read()
        c.close()
        return r.status, data, bid, sha

    def rows(self):
        return sqlite3.connect(self.cfg["db"]).execute("SELECT ep,ev,sub,line FROM records ORDER BY ep,ev,sub").fetchall()

    def raw(self, data, shut=True, read=True):
        s = socket.create_connection(("127.0.0.1", self.port), timeout=5)
        s.sendall(data)
        if shut:
            s.shutdown(socket.SHUT_WR)
        out = b""
        if read:
            with contextlib.suppress(OSError):
                while chunk := s.recv(4096):
                    out += chunk
        s.close()
        return out

    def head(self, cl, extra=""):
        return ("POST /v1/batch HTTP/1.1\r\nHost: x\r\nAuthorization: Bearer %s\r\nX-Device: %s\r\n%sContent-Length: %s\r\n\r\n"
                % (TOKEN, DEV, extra, cl)).encode()


class TestProtocol(Base):
    def test_commit_ack_and_verbatim(self):
        body = rec(1, 1) + rec(1, 2, 0, "gap", b'"why":"first_run","from":0,"to":0')
        st, data, bid, sha = self.post(body)
        self.assertEqual(st, 200)
        self.assertEqual(data, S.ack_bytes(bid, sha, 2, 2, 0))
        self.assertEqual(data, b'{"v":1,"id":"%s","sha256":"%s","count":2,"new":2,"dup":0}' % (bid.encode(), sha.encode()))
        self.assertEqual([r[3] for r in self.rows()], [rec(1, 1), body.split(b"\n")[1] + b"\n"])
        self.assertIn(b"999999", self.rows()[0][3])  # dur is never normalised

    def test_real_fixture_lines(self):
        if not os.path.exists(FIXTURE):
            self.skipTest("fixture absent")
        body = open(FIXTURE, "rb").read()
        st, data, *_ = self.post(body[: body.index(b"\n") + 1])
        self.assertEqual(st, 200, data)

    def test_replay_returns_identical_ack_and_lost_ack(self):
        body = rec(1, 1) + rec(1, 2)
        a = self.post(body)
        with mock.patch.dict(os.environ, {"AWSYNC_TEST_DROP_RESPONSE": "1"}):
            with self.assertRaises(Exception):
                self.post(rec(2, 1))
        self.assertEqual(len(self.rows()), 3)  # committed although the response was dropped
        b = self.post(body)
        self.assertEqual(a[:2], b[:2])
        st, data, *_ = self.post(rec(2, 1))
        self.assertEqual((st, json.loads(data)["new"]), (200, 1))  # stored ack, not recomputed
        st, data, *_ = self.post(rec(2, 1) + rec(2, 2), off=7)
        self.assertEqual((json.loads(data)["new"], json.loads(data)["dup"]), (1, 1))
        self.assertEqual(len(self.rows()), 4)

    def test_overlap_counts(self):
        self.post(rec(1, 1) + rec(1, 2))
        st, data, *_ = self.post(rec(1, 2) + rec(1, 3), off=5)
        self.assertEqual((st, json.loads(data)["new"], json.loads(data)["dup"]), (200, 1, 1))
        st, data, *_ = self.post(rec(1, 4) + rec(1, 4), off=9)  # identical twice inside one batch
        self.assertEqual((st, json.loads(data)["new"], json.loads(data)["dup"]), (200, 1, 1))

    def test_batch_conflict_same_id_other_hash(self):
        body = rec(1, 1)
        bid, *_ = self.post(body)[2:3]
        other = rec(1, 2)
        st, data, *_ = self.post(other, bid=bid)
        self.assertEqual((st, json.loads(data)["key"]), (409, "batch"))
        self.assertEqual(len(self.rows()), 1)

    def test_record_conflict_no_overwrite_and_rollback(self):
        self.post(rec(1, 1))
        body = rec(1, 5) + rec(1, 1, extra=b'"dur":1')
        st, data, *_ = self.post(body, off=3)
        self.assertEqual((st, json.loads(data)), (409, {"v": 1, "error": "conflict", "key": "1/1/1"}))
        self.assertEqual(self.rows(), [(1, 1, 1, rec(1, 1))])  # first row of the failed batch rolled back
        st, data, *_ = self.post(rec(1, 7) + rec(1, 7, extra=b'"dur":2'), off=4)  # conflict inside one batch
        self.assertEqual((st, json.loads(data)["key"]), (409, "1/7/1"))
        self.assertEqual(len(self.rows()), 1)

    def test_atomic_on_bad_last_line(self):
        bad = rec(1, 3)[:-10] + b'ffffffff"}\n'
        st, data, *_ = self.post(rec(1, 1) + rec(1, 2) + bad)
        self.assertEqual((st, json.loads(data)["error"]), (400, "line_crc"))
        self.assertEqual(self.rows(), [])

    def test_bad_lines(self):
        cases = {
            "bad_crc": rec(1, 1)[:-10] + b'ffffffff"}\n',
            "upper_crc": b'{"v":1,"t":"session","ep":1,"ev":1,"sub":1,"dur":1,"c":"%08X"}\n' % S.zlib.crc32(b'{"v":1,"t":"session","ep":1,"ev":1,"sub":1,"dur":1'),
            "dup_key": seal(b'{"v":1,"t":"session","ep":1,"ev":1,"sub":1,"ep":2'),
            "bool_ep": seal(b'{"v":1,"t":"session","ep":true,"ev":1,"sub":1'),
            "bool_sub": seal(b'{"v":1,"t":"session","ep":1,"ev":1,"sub":true'),
            "bool_v": seal(b'{"v":1,"t":"session","ep":1,"ev":1,"sub":1,"v":true'),
            "float_ep": seal(b'{"v":1,"t":"session","ep":1.0,"ev":1,"sub":1'),
            "float_other": seal(b'{"v":1,"t":"session","ep":1,"ev":1,"sub":1,"dur":1.5'),
            "nan": seal(b'{"v":1,"t":"session","ep":1,"ev":1,"sub":1,"x":NaN'),
            "ep0": rec(0, 1), "ep_big": rec(2**32, 1), "ev_neg": rec(1, -1), "ev_big": rec(1, 2**31),
            "sub2": rec(1, 1, 2), "gap_sub1": rec(1, 1, 1, "gap"), "session_sub0": rec(1, 1, 0),
            "type": rec(1, 1, 1, "other"), "v2": seal(b'{"v":2,"t":"session","ep":1,"ev":1,"sub":1'),
            "not_first": seal(b'{"t":"session","v":1,"ep":1,"ev":1,"sub":1'),
            "str_ep": seal(b'{"v":1,"t":"session","ep":"1","ev":1,"sub":1'),
            "missing_key": seal(b'{"v":1,"t":"session","ep":1,"sub":1'),
            "garbage": seal(b'{"v":1,"t":"session",ep:1'),
            "no_suffix": b'{"v":1,"t":"session","ep":1,"ev":1,"sub":1}\n',
            "nested_c": seal(b'{"v":1,"t":"session","ep":1,"ev":1,"sub":1,"x":{"a":1'),
            "long": rec(1, 1, extra=b'"pad":"' + b"x" * 400 + b'"'),
        }
        for name, line in cases.items():
            with self.subTest(name):
                st, data, *_ = self.post(line, cnt=1)
                self.assertEqual(st, 400, (name, data))
                self.assertLessEqual(len(data), 512)
        self.assertEqual(self.rows(), [])

    def test_exactly_384_ok_385_bad(self):
        base = len(rec(1, 1, extra=b'"p":""'))
        ok = rec(1, 1, extra=b'"p":"' + b"x" * (S.MAX_LINE - base) + b'"')
        self.assertEqual(len(ok), 384)
        self.assertEqual(self.post(ok)[0], 200)
        self.assertEqual(self.post(rec(1, 2, extra=b'"p":"' + b"x" * (S.MAX_LINE - base + 1) + b'"'))[0], 400)

    def test_framing_and_header_errors(self):
        body = rec(1, 1)
        self.assertEqual(self.post(body + b"x")[0], 400)  # unterminated
        self.assertEqual(self.post(body + b"\n")[0], 400)  # blank line
        self.assertEqual(self.post(body, cnt=2)[0], 400)  # count lies
        self.assertEqual(self.post(body, hdrs={"X-Batch-Sha256": "0" * 64})[0], 400)
        self.assertEqual(self.post(body, hdrs={"X-Batch-Count": "2"})[0], 400)  # id disagrees
        self.assertEqual(self.post(body, hdrs={"X-Batch-Id": "zz"})[0], 400)
        self.assertEqual(self.post(b"", cnt=0)[0], 400)
        self.assertEqual(self.post(b"".join(rec(1, i) for i in range(257)), cnt=257)[0], 400)
        r = self.raw(self.head(0) .replace(b"/v1/batch", b"/other"))
        self.assertTrue(r.startswith(b"HTTP/1.1 404"))
        self.assertEqual(self.rows(), [])

    def test_max_records_ok(self):
        self.assertEqual(self.post(b"".join(rec(1, i) for i in range(256)))[0], 200)

    def test_auth_and_device(self):
        body = rec(1, 1)
        self.assertEqual(self.post(body, token="x" * 20)[0], 401)
        self.assertEqual(self.post(body, hdrs={"Authorization": "Bearer"})[0], 401)
        self.assertEqual(self.post(body, hdrs={"Authorization": TOKEN})[0], 401)
        self.assertEqual(self.post(body, dev="11111111-1111-1111-1111-111111111111")[0], 403)
        self.assertEqual(self.post(body, dev="x")[0], 403)
        self.assertEqual(self.post(body, token="x" * 20, dev="x")[0], 401)  # auth before device
        self.assertEqual(self.rows(), [])
        data = self.post(body, token="x" * 20)[1]
        self.assertNotIn(TOKEN.encode(), data)

    def test_oversize_rejected_before_body(self):
        r = self.raw(self.head(S.MAX_BODY + 1))
        self.assertTrue(r.startswith(b"HTTP/1.1 413"), r)
        self.assertTrue(self.raw(self.head(S.MAX_BODY)).startswith(b"HTTP/1.1 400"))  # size ok, body missing

    def test_chunked_and_missing_length_rejected(self):
        h = b"POST /v1/batch HTTP/1.1\r\nHost: x\r\nAuthorization: Bearer %s\r\nX-Device: %s\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n" % (TOKEN.encode(), DEV.encode())
        self.assertTrue(self.raw(h).startswith(b"HTTP/1.1 400"))
        h = b"POST /v1/batch HTTP/1.1\r\nHost: x\r\n\r\n"
        self.assertTrue(self.raw(h).startswith(b"HTTP/1.1 400"))
        self.assertTrue(self.raw(self.head("-5")).startswith(b"HTTP/1.1 400"))
        self.assertTrue(self.raw(self.head("1", "Content-Length: 1\r\n")).startswith(b"HTTP/1.1 400"))

    def test_truncated_body_commits_nothing(self):
        body = rec(1, 1)
        bid, sha, h = batch_headers(body, 1)
        req = self.head(len(body), "".join("%s: %s\r\n" % kv for kv in h.items() if kv[0].startswith("X-Batch")))
        self.raw(req + body[:-20])
        self.assertEqual(self.rows(), [])
        self.assertEqual(self.post(body)[0], 200)  # server still healthy

    def test_stalled_body_times_out(self):
        with mock.patch.object(S, "SOCKET_TIMEOUT", 0.3):
            body = rec(1, 1)
            bid, sha, h = batch_headers(body, 1)
            req = self.head(len(body), "".join("%s: %s\r\n" % kv for kv in h.items() if kv[0].startswith("X-Batch")))
            s = socket.create_connection(("127.0.0.1", self.port), timeout=5)
            s.sendall(req + body[:10])
            t0 = time.time()
            self.assertEqual(s.recv(100), b"")  # server closed on us
            self.assertLess(time.time() - t0, 3)
            s.close()
        self.assertEqual(self.rows(), [])

    def test_total_body_deadline(self):
        with mock.patch.object(S, "BODY_DEADLINE", 0.4):
            body = rec(1, 1)
            bid, sha, h = batch_headers(body, 1)
            req = self.head(len(body), "".join("%s: %s\r\n" % kv for kv in h.items() if kv[0].startswith("X-Batch")))
            s = socket.create_connection(("127.0.0.1", self.port), timeout=5)
            s.sendall(req)
            for b in body[:-1]:  # trickle faster than the socket timeout
                try:
                    s.sendall(bytes([b]))
                except OSError:
                    break
                time.sleep(0.05)
            self.assertEqual(self.rows(), [])
            s.close()

    def test_busy_db_gives_503_without_internals(self):
        blocker = sqlite3.connect(self.cfg["db"], isolation_level=None)
        blocker.execute("BEGIN IMMEDIATE")
        with mock.patch.object(S, "BUSY_S", 0.2):
            st, data, *_ = self.post(rec(1, 1))
        self.assertEqual((st, json.loads(data)), (503, {"v": 1, "error": "busy"}))
        blocker.execute("ROLLBACK")
        blocker.close()
        self.assertEqual(self.post(rec(1, 1))[0], 200)

    def test_concurrent_batches(self):
        out = []

        def go(i):
            out.append(self.post(b"".join(rec(1, j) for j in range(i, i + 5)), off=i)[0])
        ts = [threading.Thread(target=go, args=(i,)) for i in range(0, 16, 2)]
        [t.start() for t in ts]
        [t.join() for t in ts]
        self.assertEqual(out, [200] * 8)
        self.assertEqual(len(self.rows()), 19)

    def test_restart_persistence_and_dump_keys(self):
        body = rec(2, 3) + rec(1, 9) + rec(1, 2, 0, "gap", b'"why":"first_run","from":0,"to":0')
        a = self.post(body)
        self.stop()
        self.start()
        b = self.post(body)
        self.assertEqual(a[:2], b[:2])
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            S.main(["--config", self.cfg_path, "--dump-keys", DEV])
        self.assertEqual(out.getvalue(), "1 2 0\n1 9 1\n2 3 1\n")
        self.assertEqual(sqlite3.connect(self.cfg["db"]).execute("PRAGMA journal_mode").fetchone()[0], "wal")


class TestConfig(Base):
    def load(self, **over):
        return S.load_config(self.write_config(**over), allow_loopback=True)

    def test_bind_rules(self):
        for ok in ("100.64.0.1", "100.127.255.254"):
            self.assertEqual(S.load_config(self.write_config(bind=ok, port=18766))["bind"], ok)
        for bad in ("0.0.0.0", "::", "::1", "localhost", "example.com", "192.168.1.5", "100.63.255.255", "100.128.0.0", "", 7, None, "127.0.0.1"):
            with self.subTest(bad), self.assertRaises(ValueError):
                S.load_config(self.write_config(bind=bad, port=18766))
        self.assertEqual(self.load(bind="127.0.0.1")["bind"], "127.0.0.1")
        with self.assertRaises(ValueError):
            S.load_config(self.write_config(bind="0.0.0.0"), allow_loopback=True)

    def test_other_fields(self):
        for over in ({"port": 0}, {"port": 70000}, {"port": "1"}, {"port": True}, {"devices": []}, {"devices": ["ABC"]},
                     {"devices": [DEV.upper().replace("0", "A")]}, {"devices": DEV}, {"extra": 1}):
            with self.subTest(over), self.assertRaises(ValueError):
                S.load_config(self.write_config(**{"bind": "100.64.0.1", "port": 18766, **over}))

    def test_token_rules(self):
        tok = self.cfg_path and json.load(open(self.cfg_path))["token_file"]
        for mode in (0o640, 0o604, 0o666, 0o660):
            os.chmod(tok, mode)
            with self.subTest(mode), self.assertRaises(ValueError):
                self.load()
        os.chmod(tok, 0o600)
        for bad in ("short", "a" * 15, "a" * 129, "tok en" * 4, "é" * 20, ""):
            open(tok, "w").write(bad)
            with self.subTest(bad), self.assertRaises(ValueError):
                self.load()
        open(tok, "w").write("a" * 16 + "\n")
        self.assertEqual(self.load()["token"], b"a" * 16)
        with self.assertRaises(OSError):
            self.load(token_file=tok + ".missing")

    def test_main_refuses_bad_bind(self):
        with self.assertRaises(SystemExit) as e:
            S.main(["--config", self.write_config(bind="0.0.0.0", port=18766)])
        self.assertIn("100.64.0.0/10", str(e.exception))


if __name__ == "__main__":
    unittest.main()
