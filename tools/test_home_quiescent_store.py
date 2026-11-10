"""Disposable Windows ownership/promotion controls; never launches an emulator."""
import os
from contextlib import closing
from pathlib import Path
import sqlite3
import subprocess
import sys
import tempfile
import unittest

from home_quiescent_store import Lease, digest, promote


def database(path, value):
    with closing(sqlite3.connect(path)) as db:
        db.execute('CREATE TABLE records(value TEXT, payload BLOB)')
        db.executemany('INSERT INTO records VALUES (?,zeroblob(8192))', [(value,)] * 150)
        db.commit()


def value(path):
    with closing(sqlite3.connect(path)) as db:
        assert db.execute('PRAGMA integrity_check').fetchall() == [('ok',)]
        return db.execute('SELECT DISTINCT value FROM records').fetchall()


@unittest.skipUnless(os.name == 'nt', 'Windows lease implementation')
class StoreTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='home-store-unit-',
                                               dir=Path(os.environ['LOCALAPPDATA']) / 'Temp' / 'opencode')
        self.root = Path(self.temp.name)
        self.target, self.candidate = self.root / 'app.db', self.root / 'candidate.db'
        database(self.target, 'old')
        database(self.candidate, 'new')

    def tearDown(self):
        self.temp.cleanup()

    def test_same_canonical_root_excluded_distinct_root_allowed(self):
        other = self.root / 'other'
        other.mkdir()
        with Lease(self.root), Lease(other):
            with self.assertRaises(OSError):
                Lease(other / '..')
        with Lease(self.root):
            pass

    def test_child_handoff_keeps_lock_after_parent_closes(self):
        lease = Lease(self.root)
        handle = lease.duplicate_for_child()
        startup = subprocess.STARTUPINFO()
        startup.lpAttributeList = {'handle_list': [handle]}
        try:
            child = subprocess.Popen([sys.executable, '-B', __file__, '--hold', str(self.root), str(handle)],
                                     startupinfo=startup, close_fds=True, stdin=subprocess.PIPE,
                                     stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        finally:
            lease.k.CloseHandle(handle)
        try:
            self.assertEqual(child.stdout.readline().strip(), 'owned')
            lease.close()
            with self.assertRaises(OSError):
                Lease(self.root)
            output, error = child.communicate('release\n', timeout=10)
            self.assertEqual(child.returncode, 0, error)
            with Lease(self.root):
                pass
        finally:
            lease.close()
            if child.poll() is None:
                child.kill()
                child.wait()

    def test_success_and_retained_rollback(self):
        with Lease(self.root) as lease:
            rollback = self.root / 'old.db'
            promote(lease, self.target, self.candidate, rollback, digest(self.candidate))
            self.assertEqual(value(self.target), [('new',)])
            self.assertEqual(value(rollback), [('old',)])
            with self.assertRaises(FileExistsError):
                promote(lease, self.target, self.candidate, rollback, digest(self.candidate))

    def test_cancellation_rolls_back_transaction(self):
        def cancel(status, remaining, total):
            if remaining < total - 5:
                raise RuntimeError('cancel')
        with Lease(self.root) as lease:
            with self.assertRaisesRegex(RuntimeError, 'cancel'):
                promote(lease, self.target, self.candidate, self.root / 'old.db', digest(self.candidate), cancel)
            self.assertEqual(value(self.target), [('old',)])

    def test_process_death_mid_backup_and_idempotent_recovery(self):
        result = subprocess.run([sys.executable, '-B', __file__, '--crash', str(self.root)], timeout=20)
        self.assertEqual(result.returncode, 79)
        with Lease(self.root):
            self.assertEqual(value(self.target), [('old',)])
            self.assertEqual(value(self.target), [('old',)])
            self.assertEqual(value(self.root / 'old.db'), [('old',)])

    def test_wal_committed_state_in_rollback(self):
        connection = sqlite3.connect(self.target)
        try:
            connection.execute('PRAGMA journal_mode=WAL')
            connection.execute('UPDATE records SET value="wal"')
            connection.commit()
            with Lease(self.root) as lease:
                promote(lease, self.target, self.candidate, self.root / 'old.db', digest(self.candidate))
                self.assertEqual(value(self.root / 'old.db'), [('wal',)])
                self.assertEqual(value(self.target), [('new',)])
        finally:
            connection.close()

    def test_wrong_hash_closed_lease_and_candidate_sidecar(self):
        with Lease(self.root) as lease:
            with self.assertRaisesRegex(ValueError, 'hash'):
                promote(lease, self.target, self.candidate, self.root / 'old.db', '0' * 64)
            self.assertFalse((self.root / 'old.db').exists())
            Path(str(self.candidate) + '-wal').touch()
            with self.assertRaisesRegex(ValueError, 'standalone'):
                promote(lease, self.target, self.candidate, self.root / 'old.db', digest(self.candidate))
        with self.assertRaisesRegex(ValueError, 'closed lease'):
            promote(lease, self.target, self.candidate, self.root / 'old.db', digest(self.candidate))
        self.assertEqual(value(self.target), [('old',)])

    def test_contention_aborts_without_replacing_target(self):
        writer = sqlite3.connect(self.target, timeout=0)
        try:
            writer.execute('BEGIN IMMEDIATE')
            with Lease(self.root) as lease:
                with self.assertRaisesRegex(RuntimeError, 'contention'):
                    promote(lease, self.target, self.candidate, self.root / 'old.db', digest(self.candidate))
        finally:
            writer.rollback()
            writer.close()
        self.assertEqual(value(self.target), [('old',)])

    def test_inherited_wrong_store_rejected(self):
        other = self.root / 'other'
        other.mkdir()
        with Lease(self.root) as lease:
            handle = lease.duplicate_for_child()
            with self.assertRaisesRegex(ValueError, 'different canonical store'):
                Lease.adopt(other, handle)  # rejection closes the duplicate
            lease.check()


if __name__ == '__main__':
    if len(sys.argv) > 1 and sys.argv[1] == '--hold':
        with Lease.adopt(sys.argv[2], int(sys.argv[3])):
            print('owned', flush=True)
            sys.stdin.readline()
    elif len(sys.argv) > 1 and sys.argv[1] == '--crash':
        root = Path(sys.argv[2])
        def crash(status, remaining, total):
            if remaining < total - 5:
                os._exit(79)
        with Lease(root) as lease:
            promote(lease, root / 'app.db', root / 'candidate.db', root / 'old.db', digest(root / 'candidate.db'), crash)
        raise AssertionError('crash injection was not reached')
    else:
        unittest.main()
