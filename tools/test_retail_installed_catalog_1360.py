"""Offline fixtures validate diagnostic boundaries, not native catalog delivery."""
from contextlib import closing
from pathlib import Path
import sqlite3
import struct
import tempfile
import unittest

from audit_retail_installed_catalog_1360 import USER, digest, display_predicates, native_evidence, offline_database, roster_user, verify_rows


class EvidenceTests(unittest.TestCase):
    def test_offline_counts_or_mounts_are_not_native_success(self):
        result = native_evidence('8 installed-app rows\n19 title mounts\nPPSA17221 Minecraft')
        self.assertEqual(result['status'], 'not_established')
        self.assertFalse(result['native_visibility_verified'])

    def test_initialization_rejection_requires_all_causal_categories(self):
        lines = [
            '[AppDb] InitClient DbAccessor::Init() : ret=0, ver_check_result=0x100',
            'Daemon::InitClient() error checkCorruption() err',
            'ExecQuerySQL(tbl_iconinfo) error 809c0103',
            'ExecQuerySQL(tbl_concepticoninfo) error 809c0103',
        ]
        for missing in range(4):
            self.assertEqual(native_evidence('\n'.join(lines[:missing]+lines[missing+1:]))['status'], 'not_established')
        result = native_evidence('\n'.join(lines))
        self.assertEqual(result['status'], 'native_catalog_initialization_rejected')
        self.assertEqual(result['evidence']['concept_query_failed']['first_lines'][0]['line'], 4)
        self.assertFalse(result['native_visibility_verified'])

    def test_repeated_failures_are_bounded_but_counted(self):
        result = native_evidence(('BeginTransaction error 0x809c0103\n')*100)
        row = result['evidence']['browse_transaction_failed']
        self.assertEqual(row['count'], 100)
        self.assertEqual(len(row['first_lines']), 4)

    def test_later_reader_is_not_erased_by_startup_rejection(self):
        result = native_evidence('Read 4096 bytes from: C:/private/sd/priv/mms/app.db\n')
        self.assertEqual(result['later_app_db_reads'][0]['line'], 1)
        self.assertFalse(result['native_visibility_verified'])

    def test_game_space_alone_is_not_library_or_home_switcher(self):
        row = display_predicates(dict(dispLocation=2, visible=1))
        self.assertTrue(row['game_space'])
        self.assertFalse(row['library'])
        self.assertFalse(row['home_game_switcher'])
        row = display_predicates(dict(dispLocation=2|8|128, visible=1))
        self.assertTrue(row['library'] and row['home_game_switcher'])
        self.assertTrue(row['predicate_only_not_full_query'])
        self.assertFalse(display_predicates(dict(dispLocation=138, visible=0))['library'])

    def test_saved_native_roster_not_guessed_user(self):
        data = struct.pack('<18I', USER, *([0xffffffff]*15), 0, 0)
        self.assertEqual(roster_user(data), [USER])
        for data in (b'', struct.pack('<18I', 0xffffffff, USER, *([0xffffffff]*14), 0, 0),
                     struct.pack('<18I', 0x10010000, *([0xffffffff]*15), 0, 0)):
            with self.assertRaises(ValueError):
                roster_user(data)


class DatabaseTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='retail-catalog-audit-',
            dir=Path.home()/'AppData/Local/Temp/opencode')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.db = self.root/'app.db'
        icon, concept = f'tbl_iconinfo_{USER:010d}', f'tbl_concepticoninfo_{USER:010d}'
        with closing(sqlite3.connect(self.db)) as c:
            c.executescript(f'''
                CREATE TABLE tbl_contentinfo(titleId TEXT PRIMARY KEY, installStatus, categoryType, metaDataPath, pprDeeplinkUri);
                CREATE TABLE tbl_conceptmetadata(localConceptId TEXT PRIMARY KEY);
                CREATE TABLE {icon}(titleId TEXT PRIMARY KEY, titleName, localConceptId, installStatus, visible, dispLocation, platform);
                CREATE TABLE {concept}(localConceptId TEXT PRIMARY KEY, validFlag, installedStatus);
                INSERT INTO tbl_contentinfo VALUES('PPSA17221',0,0,'/user/appmeta/PPSA17221','psgm:play?id=PPSA17221');
                INSERT INTO tbl_conceptmetadata VALUES('cid:local:PPSA17221');
                INSERT INTO {icon} VALUES('PPSA17221','Minecraft','cid:local:PPSA17221',2,1,2,0);
                INSERT INTO {concept} VALUES('cid:local:PPSA17221',1,0);
            ''')

    def test_rows_and_zero_version_remain_offline_only_and_unchanged(self):
        before = {p.name: (p.stat().st_mtime_ns, p.read_bytes()) for p in self.root.iterdir()}
        result = offline_database(self.db)
        self.assertEqual(result['rows'][0]['titleId'], 'PPSA17221')
        self.assertEqual(result['rows'][0]['iconInstallStatus'], 2)
        self.assertEqual(result['diagnostic_join_ids'], ['cid:local:PPSA17221'])
        self.assertEqual(result['user_version'], 0)  # report, never stamp or reject on guess
        self.assertFalse(result['native_visibility_verified'])
        self.assertTrue(result['offline_only'])
        self.assertEqual(before, {p.name: (p.stat().st_mtime_ns, p.read_bytes()) for p in self.root.iterdir()})

    def test_non_atomic_wal_or_journal_requires_review(self):
        for suffix in ('-wal', '-journal'):
            sidecar = self.db.with_name(self.db.name+suffix)
            sidecar.write_bytes(b'not a coherent snapshot')
            with self.assertRaisesRegex(ValueError, 'Non-atomic SQLite sidecar'):
                offline_database(self.db)
            sidecar.unlink()

    def test_corruption_marker_is_not_removed_or_rewritten(self):
        marker = self.root/'app.db-corrupt'
        marker.write_bytes(bytes.fromhex('0100e1fe'))
        offline_database(self.db)
        self.assertEqual(marker.read_bytes(), bytes.fromhex('0100e1fe'))

    def test_wrong_user_or_missing_user_table_fails_closed(self):
        with self.assertRaises(ValueError):
            offline_database(self.db, 0x10010000)
        with closing(sqlite3.connect(self.db)) as c:
            c.execute(f'DROP TABLE tbl_iconinfo_{USER:010d}')
        with self.assertRaisesRegex(ValueError, 'catalog tables missing'):
            offline_database(self.db)

    def test_malformed_sqlite_is_not_repaired(self):
        self.db.write_bytes(b'not a database')
        with self.assertRaises(sqlite3.DatabaseError):
            offline_database(self.db)
        self.assertEqual(self.db.read_bytes(), b'not a database')

    def test_receipt_change_and_escape_are_rejected(self):
        row = dict(bytes=self.db.stat().st_size, sha256=digest(self.db.read_bytes()))
        verify_rows(self.root, {'app.db': row})
        for rows in ({'app.db': dict(bytes=0, sha256=row['sha256'])}, {'../escape': row}):
            with self.assertRaises(ValueError):
                verify_rows(self.root, rows)


if __name__ == '__main__':
    unittest.main()
