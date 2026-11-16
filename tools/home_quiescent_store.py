"""Unwired Windows store-lease and SQLite candidate promotion component.

Uses the existing runtimeChildProcess .runtime-lock/share-zero convention.
Only cooperating launchers are excluded. Never call on an active guest store:
legacy/uncoordinated launch paths do not honor this protocol. No activation CLI.
Promotion uses SQLite's atomic backup transaction, not DB/sidecar renames.
"""
import ctypes
from ctypes import wintypes as w
import hashlib
import os
from pathlib import Path
import sqlite3


def kernel():
    if os.name != 'nt':
        raise OSError('Windows ownership implementation required')
    k = ctypes.WinDLL('kernel32', use_last_error=True)
    k.CreateFileW.argtypes = [w.LPCWSTR, w.DWORD, w.DWORD, ctypes.c_void_p, w.DWORD, w.DWORD, w.HANDLE]
    k.CreateFileW.restype = w.HANDLE
    k.CloseHandle.argtypes = [w.HANDLE]
    k.GetCurrentProcess.restype = w.HANDLE
    k.DuplicateHandle.argtypes = [w.HANDLE, w.HANDLE, w.HANDLE, ctypes.POINTER(w.HANDLE), w.DWORD, w.BOOL, w.DWORD]
    k.GetFinalPathNameByHandleW.argtypes = [w.HANDLE, w.LPWSTR, w.DWORD, w.DWORD]
    k.GetFinalPathNameByHandleW.restype = w.DWORD
    return k


def canonical(root):
    root = Path(root).resolve(strict=True)
    if not root.is_dir():
        raise ValueError('store root must be an existing directory')
    return root


class Lease:
    def __init__(self, root):
        self.root = canonical(root)
        self.k = kernel()
        self.handle = None
        lock = self.root / '.runtime-lock'
        if lock.is_symlink():
            raise ValueError('redirected lock file')
        handle = self.k.CreateFileW(str(lock), 0xc0000000, 0, None, 4, 0x80, None)
        if handle == ctypes.c_void_p(-1).value:
            raise ctypes.WinError(ctypes.get_last_error())
        self.handle = handle
        try:
            self.check()
        except BaseException:
            self.close()
            raise

    @classmethod
    def adopt(cls, root, handle):
        """Take ownership of an explicitly inherited handle in the child.

        Parent must duplicate before spawn, whitelist only that duplicate in
        STARTUPINFOEX, close its duplicate after spawn and retain its own lease
        until child acknowledgment. This method does not reopen the lock.
        """
        value = cls.__new__(cls)
        value.root, value.k, value.handle = canonical(root), kernel(), int(handle)
        try:
            value.check()
        except BaseException:
            value.close()
            raise
        return value

    def check(self):
        if not self.handle:
            raise ValueError('closed lease')
        buf = ctypes.create_unicode_buffer(32768)
        size = self.k.GetFinalPathNameByHandleW(self.handle, buf, len(buf), 0)
        if not size or size >= len(buf):
            raise ctypes.WinError(ctypes.get_last_error())
        actual = buf.value
        if actual.startswith('\\\\?\\UNC\\'):
            actual = '\\\\' + actual[8:]
        elif actual.startswith('\\\\?\\'):
            actual = actual[4:]
        if os.path.normcase(actual) != os.path.normcase(str(self.root / '.runtime-lock')):
            raise ValueError('inherited handle belongs to a different canonical store')

    def duplicate_for_child(self):
        self.check()
        result = w.HANDLE()
        current = self.k.GetCurrentProcess()
        if not self.k.DuplicateHandle(current, self.handle, current, ctypes.byref(result), 0, True, 2):
            raise ctypes.WinError(ctypes.get_last_error())
        return result.value

    def close(self):
        if self.handle:
            self.k.CloseHandle(self.handle)
            self.handle = None

    def __enter__(self):
        self.check()
        return self

    def __exit__(self, *args):
        self.close()


def digest(path):
    h = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            h.update(block)
    return h.hexdigest()


def connect_readonly(path):
    return sqlite3.connect(path.as_uri() + '?mode=ro', uri=True, timeout=0)


def promote(lease, target, candidate, rollback, expected_sha256, progress=None):
    """Replace a quiescent SQLite DB's contents atomically, preserving rollback.

    All files must be within the owned canonical store. Source must be a closed
    standalone candidate (no sidecars); callers still validate catalog/roster/
    source semantics before calling. Retained rollback is a SQLite backup of
    committed target state including WAL, not an unsafe raw main-file copy.
    SQLite FULL synchronization supplies the commit/rollback crash protocol.
    After interruption, reopen target with SQLite under the same lease to recover.
    No claim is made about non-SQLite files or malicious/noncooperating writers.
    """
    lease.check()
    target, candidate = Path(target).resolve(strict=True), Path(candidate).resolve(strict=True)
    rollback = Path(rollback).resolve()
    for path in (target, candidate, rollback):
        if not path.is_relative_to(lease.root) or path == lease.root:
            raise ValueError('path outside leased store')
    if len({target, candidate, rollback}) != 3 or os.path.samefile(target, candidate):
        raise ValueError('source/target/rollback alias')
    if any(Path(str(candidate) + suffix).exists() for suffix in ('-wal', '-shm', '-journal')):
        raise ValueError('candidate must be standalone and closed')
    if digest(candidate) != expected_sha256:
        raise ValueError('candidate hash mismatch')
    # Exclusive creation prevents overwriting any previous rollback artifact.
    with rollback.open('xb'):
        pass
    src = dst = old = None
    try:
        src = connect_readonly(candidate)
        if src.execute('PRAGMA integrity_check').fetchall() != [('ok',)]:
            raise ValueError('candidate integrity failure')
        dst = sqlite3.connect(target, timeout=0)
        dst.execute('PRAGMA synchronous=FULL')
        old = sqlite3.connect(rollback, timeout=0)
        old.execute('PRAGMA synchronous=FULL')
        def bounded(status, remaining, total):
            if status in (sqlite3.SQLITE_BUSY, sqlite3.SQLITE_LOCKED):
                raise RuntimeError('unexpected SQLite contention under store lease')
        dst.backup(old, progress=bounded, sleep=0)
        old.close()
        old = None
        with rollback.open('r+b') as stream:
            os.fsync(stream.fileno())
        if digest(candidate) != expected_sha256:
            raise ValueError('candidate changed before promotion')
        lease.check()
        # Backup destination is one SQLite write transaction. Exceptions in
        # progress abort it; process death is recovered via SQLite's journal/WAL.
        def on_page(status, remaining, total):
            bounded(status, remaining, total)
            if progress is not None:
                progress(status, remaining, total)
        src.backup(dst, pages=1, progress=on_page, sleep=0)
        if dst.execute('PRAGMA integrity_check').fetchall() != [('ok',)]:
            raise RuntimeError('promoted integrity check failed; retain rollback')
        return {'candidate_sha256': expected_sha256, 'rollback': str(rollback)}
    finally:
        for connection in (old, dst, src):
            if connection is not None:
                connection.close()
