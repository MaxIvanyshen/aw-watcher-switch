#!/usr/bin/env python3
"""Exercise the real portable uploader and receiver with synthetic data only."""
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import sqlite3
import subprocess
import tempfile
import threading
import zlib

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent
DEVICE = '11111111-1111-4111-8111-111111111111'
TOKEN = 'e2e-synthetic-not-a-real-secret-12345'
spec = importlib.util.spec_from_file_location('sync_receiver', HERE / 'server/awsync_server.py')
receiver = importlib.util.module_from_spec(spec)
spec.loader.exec_module(receiver)


def encode(record):
    record = dict(record)
    record.pop('c', None)
    body = json.dumps(record, separators=(',', ':')).encode()[:-1]
    return body + (',"c":"%08x"}\n' % zlib.crc32(body)).encode()


def main():
    subprocess.run(['make', '-C', str(HERE), 'sync_host'], check=True)
    client = ROOT / 'build/addon-sync/sync_host'
    with tempfile.TemporaryDirectory(prefix='aw-sync-e2e-') as td:
        base = Path(td)
        token = base / 'token'
        token.write_text(TOKEN)
        token.chmod(0o600)
        config = base / 'server.json'
        db = base / 'sync.db'
        config.write_text(json.dumps({'bind': '127.0.0.1', 'port': 0, 'db': str(db),
                                      'token_file': str(token), 'devices': [DEVICE]}))
        server = receiver.make_server(receiver.load_config(str(config), True))
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        port = server.server_address[1]
        sd = base / 'sd'
        journal = sd / 'config/aw-watcher-switch/sessions.jsonl'
        journal.parent.mkdir(parents=True)
        rows = (ROOT / 'tests/fixtures/synthetic-diag.expected').read_bytes().splitlines(keepends=True)
        journal.write_bytes(b''.join(rows) + b'torn tail')
        client_cfg = base / 'client.txt'
        client_cfg.write_text(f'server=100.64.0.1:{port}\ndevice={DEVICE}\ntoken={TOKEN}\n')
        state = sd / 'config/aw-watcher-switch-sync'

        def run(expected=0):
            result = subprocess.run([str(client), '--root', str(sd), '--config', str(client_cfg),
                                     '--once', '--connect', f'127.0.0.1:{port}'], capture_output=True, text=True)
            if result.returncode != expected:
                raise AssertionError(f'client exit {result.returncode}, expected {expected}: {result.stderr}')

        def stored():
            with sqlite3.connect(db) as conn:
                return conn.execute('SELECT ep,ev,sub,line FROM records ORDER BY ep,ev,sub').fetchall()

        def reset_checkpoint():
            for name in ('sync.a', 'sync.b'):
                (state / name).unlink(missing_ok=True)

        try:
            run()
            assert len(stored()) == len(rows)
            original = stored()
            run()
            assert stored() == original
            reset_checkpoint()
            run()
            assert stored() == original
            print('PASS: initial delivery, idle run, and checkpoint-loss replay deduplicate')

            extra = json.loads(rows[-1])
            extra['ev'] += 10000
            with journal.open('ab') as f:
                f.write(b'\n' + encode(extra))
            before = {p.name: hashlib.sha256(p.read_bytes()).digest()
                      for p in state.glob('sync.[ab]')}
            os.environ['AWSYNC_TEST_DROP_RESPONSE'] = '1'
            try:
                run(2)
            finally:
                os.environ.pop('AWSYNC_TEST_DROP_RESPONSE', None)
            assert len(stored()) == len(rows) + 1
            after = {p.name: hashlib.sha256(p.read_bytes()).digest()
                     for p in state.glob('sync.[ab]')}
            assert before == after, 'unacknowledged batch advanced checkpoint'
            run()
            assert len(stored()) == len(rows) + 1
            print('PASS: server commit with lost acknowledgement safely retries')

            immutable = stored()
            conflict = json.loads(rows[0])
            conflict['why'] = 'state_lost' if conflict['t'] == 'gap' else 'exit'
            if encode(conflict) == rows[0]:
                conflict['why'] = 'state_corrupt' if conflict['t'] == 'gap' else 'power'
            journal.write_bytes(encode(conflict) + b''.join(rows[1:]))
            reset_checkpoint()
            run(3)
            assert stored() == immutable
            print('PASS: conflicting record returns BLOCKED and leaves SQLite unchanged')
        finally:
            server.shutdown()
            server.server_close()
            thread.join(timeout=5)
            os.environ.pop('AWSYNC_TEST_DROP_RESPONSE', None)
        reset_checkpoint()
        run(2)
        assert stored() == immutable
        print('PASS: offline receiver returns RETRY without data deletion')
    print('ALL END-TO-END CHECKS PASSED (synthetic data, loopback only)')


if __name__ == '__main__':
    main()
