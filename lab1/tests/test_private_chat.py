"""Real Windows server/client integration test. Run: python tests/test_private_chat.py"""
import concurrent.futures
import socket
import struct
import subprocess
import threading
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

def string(value):
    data = value.encode('utf-8')
    return struct.pack('!H', len(data)) + data

class Peer:
    def __init__(self, port, name):
        self.sock = socket.create_connection(('127.0.0.1', port))
        self.frames = []
        self.send(1, string(name))
        self.worker = threading.Thread(target=self.read, daemon=True)
        self.worker.start()

    def send(self, kind, payload=b''):
        self.sock.sendall(struct.pack('!4sBBHI', b'NCHT', 1, kind, 0, len(payload)) + payload)

    def exact(self, size):
        result = b''
        while len(result) < size:
            part = self.sock.recv(size - len(result))
            if not part:
                raise EOFError()
            result += part
        return result

    def read(self):
        try:
            while True:
                magic, version, kind, reserved, size = struct.unpack('!4sBBHI', self.exact(12))
                assert (magic, version, reserved) == (b'NCHT', 1, 0)
                self.frames.append((kind, self.exact(size)))
        except (EOFError, OSError):
            pass

    def close(self):
        self.sock.close()

def wait_for(predicate):
    deadline = time.monotonic() + 8
    while time.monotonic() < deadline:
        if predicate():
            return
        time.sleep(.02)
    raise AssertionError('Timed out waiting for expected frame/output')

def run():
    with socket.socket() as probe:
        probe.bind(('127.0.0.1', 0))
        port = probe.getsockname()[1]
    server = subprocess.Popen([str(ROOT / 'bin/server.exe'), str(port)], stdout=subprocess.DEVNULL)
    peers = []
    clients = []
    try:
        time.sleep(.4)
        alice, bob, charlie = [Peer(port, name) for name in ('Alice', 'Bob', 'Charlie')]
        peers += [alice, bob, charlie]
        wait_for(lambda: all(any(k == 2 for k, _ in p.frames) for p in peers))
        body = '你好 Hello  Bob with spaces'
        alice.send(10, struct.pack('!Q', 1) + string('Bob') + string(body))
        wait_for(lambda: all(any(k == 11 for k, _ in p.frames) for p in (alice, bob)))
        delivered = next(data for k, data in bob.frames if k == 11)
        assert delivered == next(data for k, data in alice.frames if k == 11)
        assert delivered.endswith(string('Alice') + string('Bob') + string(body))
        time.sleep(.1)
        assert not any(k == 11 for k, _ in charlie.frames)
        alice.send(10, struct.pack('!Q', 2) + string('Nobody') + string('secret'))
        wait_for(lambda: any(k == 8 for k, _ in alice.frames))
        before = sum(k == 8 for k, _ in alice.frames)
        for payload in (b'\0', struct.pack('!Q', 3) + string('Bob') + string('x') + b'X',
                        struct.pack('!Q', 3) + string('Bob') + string(' '),
                        struct.pack('!Q', 1) + string('Bob') + string('duplicate')):
            alice.send(10, payload)
        wait_for(lambda: sum(k == 8 for k, _ in alice.frames) == before + 4)
        bob.send(7)
        wait_for(lambda: any(k == 5 and b'Bob left' in d for k, d in alice.frames))
        alice.send(10, struct.pack('!Q', 3) + string('Bob') + string('offline secret'))
        wait_for(lambda: sum(k == 8 for k, _ in alice.frames) == before + 5)
        alice.send(10, struct.pack('!Q', 4) + string('Alice') + string('self'))
        wait_for(lambda: sum(k == 11 for k, _ in alice.frames) == 2)

        def traffic(peer, target):
            for i in range(1, 101):
                peer.send(3, struct.pack('!Q', i) + string('group'))
                peer.send(10, struct.pack('!Q', i + 10) + string(target) + string('concurrent secret'))
        with concurrent.futures.ThreadPoolExecutor() as pool:
            list(pool.map(lambda args: traffic(*args), [(alice, 'Charlie'), (charlie, 'Alice')]))
        wait_for(lambda: all(sum(k == 4 for k, _ in p.frames) == 200 for p in (alice, charlie)))
        wait_for(lambda: sum(k == 11 for k, _ in charlie.frames) == 200)
        assert sum(k == 11 for k, _ in alice.frames) == 202
        alice.send(9)
        wait_for(lambda: any(k == 6 and b'Charlie' in d for k, d in alice.frames))

        # Exercise the actual C++ input parser, display and /burst regression.
        outputs = {}
        for name in ('UIAlice', 'UIBob', 'UICharlie'):
            proc = subprocess.Popen([str(ROOT/'bin/client.exe'), '127.0.0.1', str(port), name],
                                    stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            clients.append(proc)
            outputs[name] = bytearray()
            def collect(p=proc, result=outputs[name]):
                while True:
                    chunk = p.stdout.read(1)
                    if not chunk:
                        break
                    result.extend(chunk)
            threading.Thread(target=collect, daemon=True).start()
        wait_for(lambda: all(b'Welcome,' in result for result in outputs.values()))
        proc = clients[0]
        proc.stdin.write('/help\n/msg\n/msg UIBob\n/msg UIBob    \n/msg UIBob 你好 Hello  Bob\nfirst group\n/burst 100\n/list\n'.encode())
        proc.stdin.flush()
        wait_for(lambda: b'[PRIVATE OUT]' in outputs['UIAlice'] and b'[PRIVATE IN]' in outputs['UIBob'])
        wait_for(lambda: all(b'[TEST] burst 100/100' in result for result in outputs.values()))
        assert outputs['UIAlice'].count(b'Usage: /msg') == 3
        assert '你好 Hello  Bob'.encode() in outputs['UIBob']
        assert b'[PRIVATE' not in outputs['UICharlie']
        assert all(b'application-message gap' not in result for result in outputs.values())
        assert b'/msg USER MESSAGE' in outputs['UIAlice']
        for proc in clients:
            proc.stdin.write(b'/quit\n'); proc.stdin.flush()
            assert proc.wait(timeout=8) == 0
        print('PASS: real server, three protocol peers, three C++ clients; private routing, UTF-8/spaces, offline/missing target, malformed/replayed payloads, self delivery, 200 concurrent group + 200 private requests, help/list/burst100/quit/join/leave.')
    finally:
        for proc in clients:
            if proc.poll() is None:
                proc.kill()
            proc.wait()
        for peer in peers:
            peer.close()
        server.terminate(); server.wait(timeout=8)

if __name__ == '__main__':
    run()
