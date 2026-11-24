"""Terminal for the USB key: shows its log and sends one-letter commands.

Usage: python3 -I tools/usbkey/console.py [--port /dev/cu.usbmodemNNNN] [--log F]

Without --port it asks the I/O registry (ioreg) for the serial port of the USB device named
"JieLi USB key (RP2040)", so it never opens another device's port. Every line
from the key goes to the screen and, with --log, to a JSON Lines file with a host timestamp (ns),
as the other tools log. Keys: r start an attempt, x abort, d replug, s status, h help; q or Ctrl-C
leaves (the key keeps running). Standard library only.
"""
import json
import os
import re
import select
import subprocess
import sys
import termios
import time
import tty


KEY_NAME = "JieLi USB key (RP2040)"  # the product string in usb_descriptors.c


def find_key_ports():
    """Serial ports (/dev/cu.*) below USB devices named like the key."""
    out = subprocess.run(["ioreg", "-r", "-n", KEY_NAME, "-l", "-w0"],
                         capture_output=True, text=True, check=False).stdout
    return sorted(set(re.findall(r'"IOCalloutDevice" = "([^"]+)"', out)))


def open_port(path):
    fd = os.open(path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    # Raw at once and without a flush: opening sets DTR, and the key answers with everything it
    # logged since power-up (or since the terminal was last open). tty.setraw()'s default,
    # TCSAFLUSH, threw that away (seen on the first run, 2026-11-09).
    tty.setraw(fd, termios.TCSANOW)
    attrs = termios.tcgetattr(fd)
    attrs[2] |= termios.HUPCL | termios.CLOCAL | termios.CREAD  # DTR on while open: the key sends its log
    termios.tcsetattr(fd, termios.TCSANOW, attrs)
    return fd


def main(argv):
    args = list(argv)

    def opt(name):
        if name in args:
            i = args.index(name)
            v = args[i + 1]
            del args[i:i + 2]
            return v
        return None

    port, log_path = opt("--port"), opt("--log")
    if args:
        sys.exit(__doc__.strip())
    if port is None:
        ports = find_key_ports()
        if len(ports) != 1:
            sys.exit("console: " + (f"no USB device named \"{KEY_NAME}\" with a serial port" if not ports else
                                    "more than one key, pick with --port: " + " ".join(ports)))
        port = ports[0]
    fd = open_port(port)
    log = open(log_path, "a") if log_path else None

    def record(event, **kw):
        if log:
            log.write(json.dumps(dict(t_ns=time.time_ns(), event=event, **kw), sort_keys=True) + "\n")
            log.flush()

    record("start", port=port)
    print(f"console: {port}  (r start, x abort, d replug, s status, h help, q quit)\r")
    stdin = sys.stdin.fileno()
    saved = termios.tcgetattr(stdin) if os.isatty(stdin) else None
    if saved:
        tty.setcbreak(stdin)
    pending = b""
    try:
        while True:
            ready, _, _ = select.select([fd, stdin], [], [], 0.5)
            if fd in ready:
                try:
                    data = os.read(fd, 4096)
                except BlockingIOError:
                    data = b""
                except OSError as e:
                    print(f"\nconsole: the port went away ({e})")
                    record("port_lost", error=str(e))
                    break
                pending += data
                while b"\n" in pending:
                    line, pending = pending.split(b"\n", 1)
                    text = line.decode("utf-8", "replace").rstrip("\r")
                    print(text)
                    record("line", text=text)
            if stdin in ready:
                c = os.read(stdin, 1)
                if not c or c in (b"q", b"\x03"):
                    break
                os.write(fd, c)
                record("sent", key=c.decode("ascii", "replace"))
    except KeyboardInterrupt:
        pass
    finally:
        if saved:
            termios.tcsetattr(stdin, termios.TCSADRAIN, saved)
        record("stop")
        os.close(fd)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
