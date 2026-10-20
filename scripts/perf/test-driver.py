#!/usr/bin/env python3
"""Black-box checks of success, wrong content, scheduler misses and socket deadline."""
import http.server
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import threading
import time


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'
    body = b'[TEST]'
    delay = 0

    def do_GET(self):
        time.sleep(type(self).delay)
        try:
            self.send_response(200)
            self.send_header('Content-Length', str(len(type(self).body)))
            self.end_headers()
            self.wfile.write(type(self).body)
        except (BrokenPipeError, ConnectionResetError):
            pass

    def log_message(self, *args):
        pass


def main():
    with http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler) as server, tempfile.TemporaryDirectory() as temp:
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        env = os.environ | {'PERF_GATE_PORT': str(server.server_port)}
        for name, scenario, body, delay, rate, duration in [
                ('success', 'http-test', b'[TEST]', 0, 10, .5),
                ('keepalive', 'http-test-keepalive', b'[TEST]', 0, 10, .5),
                ('wrong', 'http-test', b'wrong', 0, 10, .5),
                ('slow', 'http-test', b'[TEST]', .12, 50, .5),
                ('timeout', 'http-test', b'[TEST]', 5, 1, .5)]:
            Handler.body, Handler.delay = body, delay
            path = Path(temp) / f'{name}.jsonl'
            started = time.monotonic()
            result = subprocess.run([sys.argv[1], scenario, str(rate), str(duration), '1', str(path)],
                                    env=env, capture_output=True, text=True, timeout=8)
            rows = [json.loads(line) for line in path.read_text().splitlines()]
            summary = json.loads(result.stdout)
            assert len(rows) == summary['planned'], (name, result)
            if name in {'success', 'keepalive'}:
                assert result.returncode == 0 and all(row['success'] for row in rows)
            else:
                assert result.returncode == 1 and any(not row['success'] for row in rows)
            if name == 'slow':
                assert summary['generator_missed'] > 0
            if name == 'timeout':
                assert time.monotonic() - started < 4.5
        server.shutdown()
    print('driver qualification passed')


if __name__ == '__main__':
    main()
