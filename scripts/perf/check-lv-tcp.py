#!/usr/bin/env python3
"""Verify LV TCP framing behavior against a running Gateway."""

import socket
import struct
import sys
import time

from pathlib import Path

SMOKE_SRC = Path(__file__).resolve().parents[1] / 'smoke' / 'src'
sys.path.insert(0, str(SMOKE_SRC))

from imm_tcp_client import ClientEnvelope, Operation, encode_envelope  # noqa: E402


def frame(payload):
    return struct.pack('!I', len(payload)) + payload


def recv_exact(sock, size):
    chunks = []
    while size:
        chunk = sock.recv(size)
        if not chunk:
            raise EOFError('connection closed before a complete frame')
        chunks.append(chunk)
        size -= len(chunk)
    return b''.join(chunks)


def recv_response(sock, request_id):
    size = struct.unpack('!I', recv_exact(sock, 4))[0]
    envelope = ClientEnvelope()
    envelope.ParseFromString(recv_exact(sock, size))
    assert envelope.HasField('response')
    assert envelope.response.request_id == request_id
    assert envelope.response.operation == Operation.OP_PING


def ping(request_id):
    return frame(encode_envelope(Operation.OP_PING, request_id, {}))


def expect_close(port, source):
    with socket.create_connection(('127.0.0.1', port), timeout=2) as sock:
        sock.settimeout(2)
        source(sock)
        assert sock.recv(1) == b''


def main():
    port = int(sys.argv[1])

    with socket.create_connection(('127.0.0.1', port), timeout=2) as sock:
        sock.settimeout(2)
        fragmented = ping('fragmented')
        for byte in fragmented:
            sock.sendall(bytes([byte]))
            time.sleep(0.0005)
        recv_response(sock, 'fragmented')

        sock.sendall(ping('coalesced-1') + ping('coalesced-2'))
        recv_response(sock, 'coalesced-1')
        recv_response(sock, 'coalesced-2')

        sock.sendall(struct.pack('!I', 0))
        sock.sendall(ping('after-empty'))
        recv_response(sock, 'after-empty')

        sock.sendall(frame(b'\xff\xff\xff'))
        sock.sendall(ping('after-malformed'))
        recv_response(sock, 'after-malformed')

    expect_close(port, lambda sock: sock.sendall(struct.pack('!I', 10 * 1024 * 1024 + 1)))

    def truncated(sock):
        sock.sendall(struct.pack('!I', 32) + b'short')
        sock.shutdown(socket.SHUT_WR)

    expect_close(port, truncated)
    print('LV TCP framing qualification passed')


if __name__ == '__main__':
    main()
