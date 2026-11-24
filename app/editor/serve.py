"""Serves the repository root on http://localhost:8765/ for the configuration editor.

The editor reads config/schema.json and config/factory.json with fetch(), which a page opened
as a file:// URL may not do, so it needs a server. Standard library only. Unlike
`python3 -m http.server`, this one skips the reverse DNS lookup that can hang its start.

    python3 -I app/editor/serve.py            # then open http://localhost:8765/app/editor/

The firmware built for the host, for the page (docs/integration.md, T-4, "The page against
devhost"): http://localhost:8765/app/editor/?devhost opens a WebSocket to /devhost here, and
each such connection starts its own build/host/devhost (make -C firmware sim) with a fresh flash
file: the controller's link, configuration protocol, control path, core and stores. The page's own
MixerDevice talks to it through a stand-in for Web MIDI (js/devhost.js): devhost's config port
(--sysex-fd, a socket pair) and its performance port (--midi, a FIFO) go both ways as binary
WebSocket messages, the first byte the port (0 the config port, 1 the performance port). POSIX
only (devhost, the pseudo-terminal, the FIFO); nothing here talks to a device. Add ?selftest to
run the page's checklist against it (js/selftest.js). devhost also gets a mixer's first 512 KB as
installed (--boot, made by firmware/tests/bootsel_vec.py --installed), so the Mixer sheet's way back
to M-Vave's firmware runs: devhost then exits as the mixer would restart.
"""
import base64
import functools
import hashlib
import http.server
import os
import select
import socket
import socketserver
import struct
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DEVHOST = os.environ.get("SMC_DEVHOST") or os.path.join(ROOT, "build", "host", "devhost")
WS_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"


class WebSocket:
    """The server's side of RFC 6455, enough for binary messages: unmasked frames out, masked in."""

    def __init__(self, sock):
        self.sock = sock
        self.buf = b""

    def send(self, data, opcode=0x2):
        n = len(data)
        head = bytes([0x80 | opcode]) + (bytes([n]) if n < 126 else struct.pack(">BH", 126, n) if n < 65536
                                         else struct.pack(">BQ", 127, n))
        self.sock.sendall(head + data)

    def feed(self, data):
        """Complete messages from the bytes received: [(opcode, payload)]."""
        self.buf += data
        out = []
        while len(self.buf) >= 2:
            b0, b1 = self.buf[0], self.buf[1]
            n, at = b1 & 0x7F, 2
            if n == 126:
                if len(self.buf) < 4:
                    break
                n, at = struct.unpack_from(">H", self.buf, 2)[0], 4
            elif n == 127:
                if len(self.buf) < 10:
                    break
                n, at = struct.unpack_from(">Q", self.buf, 2)[0], 10
            mask = b""
            if b1 & 0x80:
                mask, at = self.buf[at:at + 4], at + 4
            if len(self.buf) < at + n:
                break
            payload = self.buf[at:at + n]
            if mask:
                payload = bytes(c ^ mask[i & 3] for i, c in enumerate(payload))
            self.buf = self.buf[at + n:]
            out.append((b0 & 0x0F, payload))
        return out


def run_devhost(ws):
    """One devhost for one page, until either ends."""
    host, dev = socket.socketpair()
    master, slave = os.openpty()  # its CDC link: not used by the page, drained
    tmp = tempfile.TemporaryDirectory()
    fifo = os.path.join(tmp.name, "midi")
    os.mkfifo(fifo)
    perf = os.open(fifo, os.O_RDONLY | os.O_NONBLOCK)  # open before devhost's non-blocking write
    boot = os.path.join(tmp.name, "boot.bin")
    made = subprocess.run([sys.executable, "-I", os.path.join(ROOT, "firmware", "tests", "bootsel_vec.py"),
                           "--installed", boot], stdout=subprocess.DEVNULL).returncode == 0
    proc = subprocess.Popen([DEVHOST, "86400", "--sysex-fd", str(dev.fileno()), "--flash",
                             os.path.join(tmp.name, "flash.bin"), "--midi", fifo] + (["--boot", boot] if made else []),
                            stdin=slave, stdout=slave, pass_fds=(dev.fileno(),))
    dev.close()
    os.close(slave)
    try:
        while proc.poll() is None:
            ready, _, _ = select.select([ws.sock, host, perf, master], [], [], 0.5)
            if host in ready:
                data = host.recv(4096)
                if not data:
                    break
                ws.send(b"\x00" + data)
            if perf in ready:
                data = os.read(perf, 4096)
                if data:
                    ws.send(b"\x01" + data)
            if master in ready:
                os.read(master, 4096)
            if ws.sock in ready:
                data = ws.sock.recv(4096)
                if not data:
                    break
                for opcode, payload in ws.feed(data):
                    if opcode == 0x8:
                        return
                    if opcode == 0x9:
                        ws.send(payload, 0xA)
                    elif opcode == 0x2 and payload[:1] == b"\x00":
                        host.sendall(payload[1:])
    except OSError:
        pass
    finally:
        proc.kill()
        proc.wait()
        host.close()
        os.close(perf)
        os.close(master)
        tmp.cleanup()


class Handler(http.server.SimpleHTTPRequestHandler):
    def do_GET(self):
        if self.path.split("?")[0] != "/devhost":
            return super().do_GET()
        key = self.headers.get("Sec-WebSocket-Key")
        if self.headers.get("Upgrade", "").lower() != "websocket" or not key:
            return self.send_error(400, "a WebSocket only")
        if not os.access(DEVHOST, os.X_OK) or not hasattr(os, "mkfifo"):
            return self.send_error(503, "no build/host/devhost (make -C firmware sim), or not POSIX")
        accept = base64.b64encode(hashlib.sha1((key + WS_GUID).encode()).digest()).decode()
        self.send_response(101, "Switching Protocols")
        self.send_header("Upgrade", "websocket")
        self.send_header("Connection", "Upgrade")
        self.send_header("Sec-WebSocket-Accept", accept)
        super().end_headers()
        self.wfile.flush()
        self.close_connection = True
        print("devhost: a page connected", flush=True)
        run_devhost(WebSocket(self.connection))
        print("devhost: the page left", flush=True)

    # The editor changes while it is developed: never let the browser keep an old copy.
    def end_headers(self):
        self.send_header("Cache-Control", "no-store")
        super().end_headers()


class Server(http.server.ThreadingHTTPServer):
    def server_bind(self):
        socketserver.TCPServer.server_bind(self)
        self.server_name, self.server_port = "localhost", self.server_address[1]


def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8765
    handler = functools.partial(Handler, directory=ROOT)
    with Server(("127.0.0.1", port), handler) as srv:
        print("http://localhost:%d/app/editor/  (serving %s; Ctrl-C stops)" % (port, ROOT), flush=True)
        try:
            srv.serve_forever()
        except KeyboardInterrupt:
            pass


if __name__ == "__main__":
    main()
