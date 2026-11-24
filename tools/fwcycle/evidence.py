"""Private, fsynced run evidence. Locks are kernel-owned, never PID-killed."""
from contextlib import contextmanager
import fcntl
import json
import os
from pathlib import Path
import stat
import time
import uuid


class EvidenceError(RuntimeError):
    pass


def safe_path(path):
    path = Path(os.path.abspath(path))
    for part in (path, *path.parents):
        if part.is_symlink():
            raise EvidenceError('symlink evidence path refused')
    return path


@contextmanager
def project_lock(path):
    path = safe_path(path)
    fd = os.open(path, os.O_RDWR | os.O_CREAT | os.O_NOFOLLOW, 0o600)
    try:
        try:
            fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError as exc:
            raise EvidenceError('another project firmware-cycle writer is active') from exc
        yield
    finally:
        os.close(fd)


class Evidence:
    def __init__(self, root):
        self.root = safe_path(root)
        self.lock_fd = None
        if not self.root.is_dir() or self.root.stat().st_mode & 0o077:
            raise EvidenceError('run directory must be private (0700)')
        try:
            self.lock_fd = os.open(self.root / '.lock', os.O_CREAT | os.O_RDWR | os.O_NOFOLLOW, 0o600)
            fcntl.flock(self.lock_fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError as exc:
            self.close()
            raise EvidenceError('run already owned or lock unavailable') from exc
        try:
            records = self.records()
            self.seq = len(records)
        except BaseException:
            self.close()
            raise

    @classmethod
    def create(cls, root, metadata):
        root = safe_path(root)
        root.mkdir(mode=0o700, parents=False, exist_ok=False)
        obj = cls(root)
        try:
            obj.write_json('run.json', metadata)
        except BaseException:
            obj.close()
            raise
        return obj

    @classmethod
    def open(cls, root):
        return cls(root)

    def close(self):
        if self.lock_fd is not None:
            os.close(self.lock_fd)
            self.lock_fd = None

    def __enter__(self):
        return self

    def __exit__(self, *args):
        self.close()

    def path(self, name):
        p = Path(name)
        if p.is_absolute() or '..' in p.parts or not p.parts:
            raise EvidenceError('invalid evidence filename')
        result = safe_path(self.root / p)
        if result.parent != self.root:
            raise EvidenceError('evidence files must be direct children')
        return result

    def new_attempt(self):
        parent = safe_path(self.root / 'attempts')
        parent.mkdir(mode=0o700, exist_ok=True)
        for number in range(1, 1000000):
            path = parent / ('%04d' % number)
            try:
                path.mkdir(mode=0o700)
                return path
            except FileExistsError:
                continue
        raise EvidenceError('attempt namespace exhausted')

    def write_bytes(self, name, content):
        dest = self.path(name)
        temp = self.root / ('.atomic-' + uuid.uuid4().hex)
        try:
            fd = os.open(temp, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
            with os.fdopen(fd, 'wb') as out:
                out.write(content)
                out.flush()
                os.fsync(out.fileno())
            os.replace(temp, dest)
            fd = os.open(self.root, os.O_RDONLY)
            try:
                os.fsync(fd)
            finally:
                os.close(fd)
        finally:
            if temp.exists():
                temp.unlink()

    def write_json(self, name, value):
        self.write_bytes(name, (json.dumps(value, sort_keys=True) + '\n').encode())

    def read_json(self, name):
        fd = os.open(self.path(name), os.O_RDONLY | os.O_NOFOLLOW)
        with os.fdopen(fd, 'r') as src:
            return json.load(src)

    def save_status(self, value):
        self.write_json('status.json', value)

    def read_status(self):
        return self.read_json('status.json')

    def records(self):
        path = self.path('journal.jsonl')
        if not path.exists():
            return []
        fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW)
        with os.fdopen(fd, 'rb') as src:
            raw = src.read()
        if raw and not raw.endswith(b'\n'):
            raise EvidenceError('torn journal; reconciliation required')
        records = [json.loads(line) for line in raw.splitlines()]
        if [r.get('seq') for r in records] != list(range(1, len(records) + 1)):
            raise EvidenceError('non-contiguous journal sequence')
        return records

    def append(self, kind, data):
        record = {'seq': self.seq + 1, 'kind': kind, 'data': data,
                  'wall_ns': time.time_ns(), 'monotonic_ns': time.monotonic_ns()}
        fd = os.open(self.path('journal.jsonl'), os.O_WRONLY | os.O_APPEND |
                     os.O_CREAT | os.O_NOFOLLOW, 0o600)
        with os.fdopen(fd, 'ab') as out:
            out.write((json.dumps(record, sort_keys=True) + '\n').encode())
            out.flush()
            os.fsync(out.fileno())
        self.seq += 1
        return record
