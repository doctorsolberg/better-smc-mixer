#!/usr/bin/env python3
"""The editor's device code live against the firmware's own code on the host (docs/integration.md,
T-2L): build/host/devhost runs the link, the configuration protocol, the control path, the core and
the flash stores; tools/editortest/live.mjs runs the editor's MixerDevice (app/editor/js) in the
host test image's Node.js (docker/Dockerfile.host). This script joins them: devhost's config port (--sysex-fd, a socket pair) to
the container's stdin and stdout, MIDI bytes both ways as Web MIDI would carry them.

  python3 -I tools/editortest/live.py [--keep-going]    make test runs it

Without Docker, or with an image that has no Node, it says SKIPPED and exits 0 (as
tools/env/test_editor.sh); SMC_EDITOR_TESTS=required makes that an error (exit 2). SMC_NODE=path
runs that Node directly instead of the container's (tools/env/test_linux.sh, inside the image). Nothing here
talks to a device. Standard library only; needs build/host/devhost (make -C firmware sim).
"""
import hashlib
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import threading

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
# SMC_DEVHOST: another devhost build (the check of this test: one with a planted fault)
DEVHOST = os.environ.get("SMC_DEVHOST") or os.path.join(ROOT, "build", "host", "devhost")
SECONDS = "120"  # devhost's own limit: the session takes about 15 s


def skip(why):
    print("SKIPPED: the editor against devhost did not run: %s" % why, file=sys.stderr)
    return 2 if os.environ.get("SMC_EDITOR_TESTS") == "required" else 0


def image():
    """The host test image's tag, as tools/env/host_image.sh names it, or None."""
    with open(os.path.join(ROOT, "docker", "Dockerfile.host"), "rb") as f:
        tag = "better-smc-mixer-host:" + hashlib.sha256(f.read()).hexdigest()[:12]
    ok = subprocess.run(["docker", "image", "inspect", tag], stdout=subprocess.DEVNULL,
                        stderr=subprocess.DEVNULL).returncode == 0
    return tag if ok else None


def pump(read, write, done):
    """Copies bytes until the reader ends; then closes the writer's side."""
    try:
        while True:
            b = read(4096)
            if not b:
                break
            write(b)
    except OSError:
        pass
    finally:
        done()


def main():
    if not os.access(DEVHOST, os.X_OK):
        print("live.py: no %s: make -C firmware sim" % DEVHOST, file=sys.stderr)
        return 2
    if os.environ.get("SMC_NODE"):  # inside the host test image (tools/env/test_linux.sh): its own Node
        node = [os.environ["SMC_NODE"]]
    else:
        if not shutil.which("docker"):
            return skip("no docker command (docs/build.md)")
        if subprocess.run(["docker", "info"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode:
            return skip("Docker is not running")
        tag = image()
        if not tag:
            return skip("no host test image yet (tools/env/host_image.sh builds it)")
        user = "%d:%d" % (os.getuid(), os.getgid()) if hasattr(os, "getuid") else "1000:1000"
        node = ["docker", "run", "-i", "--rm", "--user", user,
                "-v", "%s:/src:ro" % ROOT, "-w", "/src", tag]
        if subprocess.run(node + ["sh", "-c", "command -v node >/dev/null"]).returncode:
            return skip("the image has no Node.js (docker/Dockerfile.host)")
        node = node + ["node"]

    host, dev = socket.socketpair()
    master, slave = os.openpty()  # devhost's CDC link: unused here, drained so it never blocks
    tmp = tempfile.TemporaryDirectory()
    boot = os.path.join(tmp.name, "boot.bin")  # an installed flash, for STOCK (going back)
    subprocess.run([sys.executable, "-I", os.path.join(ROOT, "firmware", "tests", "bootsel_vec.py"), "--installed", boot],
                   check=True)
    fw = subprocess.Popen([DEVHOST, SECONDS, "--sysex-fd", str(dev.fileno()), "--flash",
                           os.path.join(tmp.name, "flash.bin"), "--boot", boot], stdin=slave, stdout=slave,
                          pass_fds=(dev.fileno(),))
    dev.close()
    os.close(slave)
    ed = subprocess.Popen(node + ["tools/editortest/live.mjs"], stdin=subprocess.PIPE,
                          stdout=subprocess.PIPE, cwd=ROOT)

    def close_in():
        try:
            ed.stdin.close()
        except OSError:
            pass

    threads = [
        threading.Thread(target=pump, args=(host.recv, lambda b: (ed.stdin.write(b), ed.stdin.flush()), close_in)),
        threading.Thread(target=pump, args=(lambda n: os.read(ed.stdout.fileno(), n), host.sendall, lambda: None)),
        threading.Thread(target=pump, args=(lambda n: os.read(master, n), lambda b: None, lambda: None)),
    ]
    for t in threads:
        t.daemon = True
        t.start()
    try:
        code = ed.wait(timeout=float(SECONDS))
    except subprocess.TimeoutExpired:
        ed.kill()
        code = 1
        print("live.py: the editor's side did not finish in %s s" % SECONDS, file=sys.stderr)
    finally:
        alive = fw.poll() is None
        fw.kill()
        fw.wait()
        host.close()
        os.close(master)
        tmp.cleanup()
    # the session ends by pointing the mixer back at M-Vave's firmware (STOCK): devhost restarts on
    # that flash and exits with 0, as ours no longer starts; any other end is a failure
    if not alive and fw.returncode != 0:
        print("live.py: devhost ended during the session (exit %s)" % fw.returncode, file=sys.stderr)
        code = code or 1
    return code


if __name__ == "__main__":
    sys.exit(main())
