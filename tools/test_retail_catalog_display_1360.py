"""Actual offline importer/preservation tests, not native visibility acceptance."""
from contextlib import closing
import json
from pathlib import Path
import sqlite3
import tempfile
import unittest
from unittest.mock import patch

import prepare_shellui_databases_1360 as prep

USER = 0x10010029
GAME = dict(titleId='PPSA17221', applicationCategoryType=0)
ICON = f'tbl_iconinfo_{USER:010d}'
CONCEPT = f'tbl_concepticoninfo_{USER:010d}'


class PolicyTests(unittest.TestCase):
    def test_game_default_requires_three_independent_display_bits(self):
        self.assertEqual(prep.descriptor_display_location(GAME, True), 138)
        self.assertEqual(138 & 2, 2)
        self.assertEqual(138 & 8, 8)
        self.assertEqual(138 & 128, 128)

    def test_supplied_flags_preserved_even_if_intentionally_game_only(self):
        for flags in (0, 2, 8, 128, 146, 188, 4096):
            param = dict(GAME, displayLocation=flags)
            self.assertEqual(prep.descriptor_display_location(param, True), flags)
            self.assertEqual(prep.descriptor_display_location(param, False), flags)

    def test_no_guess_for_other_categories_or_nonexternal(self):
        for param, external in ((GAME, False), (dict(GAME, applicationCategoryType=65536), True)):
            with self.assertRaises(KeyError):
                prep.descriptor_display_location(param, external)


class RepairTests(unittest.TestCase):
    def setUp(self):
        self.conn = sqlite3.connect(':memory:')
        self.addCleanup(self.conn.close)
        self.conn.executescript(f'''
            CREATE TABLE tbl_contentinfo(titleId,categoryType,metaDataPath);
            CREATE TABLE {ICON}(titleId,localConceptId,dispLocation,visible,installStatus,entEntitlement);
            CREATE TABLE {CONCEPT}(primaryTitleId,localConceptId,metadataPath,dispLocation,validFlag,installedStatus);
            INSERT INTO tbl_contentinfo VALUES('PPSA17221',0,'/user/appmeta/PPSA17221');
            INSERT INTO {ICON} VALUES('PPSA17221','cid:local:PPSA17221',2,1,2,0);
            INSERT INTO {CONCEPT} VALUES('PPSA17221','cid:local:PPSA17221','/user/appmeta/PPSA17221',2,1,0);
        ''')

    def rows(self):
        return {t: self.conn.execute('SELECT * FROM '+t).fetchall() for t in ('tbl_contentinfo', ICON, CONCEPT)}

    def test_repairs_two_cells_only_and_is_idempotent(self):
        before = self.rows()
        with self.conn:
            self.assertEqual(prep.repair_default_game_display(self.conn, USER, GAME), 2)
        after = self.rows()
        expected = {k:list(v) for k,v in before.items()}
        expected[ICON] = [(*before[ICON][0][:2], 138, *before[ICON][0][3:])]
        expected[CONCEPT] = [(*before[CONCEPT][0][:3], 138, *before[CONCEPT][0][4:])]
        self.assertEqual(after, expected)
        with self.conn:
            self.assertEqual(prep.repair_default_game_display(self.conn, USER, GAME), 0)

    def test_explicit_descriptor_and_non_game_not_repaired(self):
        before = self.rows()
        for param in (dict(GAME, displayLocation=2), dict(GAME, applicationCategoryType=65536)):
            self.assertEqual(prep.repair_default_game_display(self.conn, USER, param), 0)
        self.assertEqual(self.rows(), before)

    def test_customized_flags_preserved(self):
        self.conn.execute(f'UPDATE {ICON} SET dispLocation=4096')
        self.conn.execute(f'UPDATE {CONCEPT} SET dispLocation=0')
        before = self.rows()
        self.assertEqual(prep.repair_default_game_display(self.conn, USER, GAME), 0)
        self.assertEqual(self.rows(), before)

    def test_nonlocal_content_binding_preserved(self):
        for category, metadata in ((65536, '/user/appmeta/PPSA17221'), (0, '/custom/path')):
            self.conn.execute('UPDATE tbl_contentinfo SET categoryType=?,metaDataPath=?', (category,metadata))
            before = self.rows()
            self.assertEqual(prep.repair_default_game_display(self.conn, USER, GAME), 0)
            self.assertEqual(self.rows(), before)

    def test_different_concept_identity_is_not_rewritten(self):
        self.conn.execute(f"UPDATE {ICON} SET localConceptId='custom-concept'")
        self.conn.execute(f"UPDATE {CONCEPT} SET localConceptId='custom-concept'")
        before = self.rows()
        self.assertEqual(prep.repair_default_game_display(self.conn, USER, GAME), 0)
        self.assertEqual(self.rows(), before)

    def test_transaction_failure_rolls_back_first_update(self):
        self.conn.executescript(f"CREATE TRIGGER reject_repair BEFORE UPDATE ON {CONCEPT} BEGIN SELECT RAISE(ABORT,'fixture reject'); END;")
        before = self.rows()
        with self.assertRaises(sqlite3.IntegrityError), self.conn:
            prep.repair_default_game_display(self.conn, USER, GAME)
        self.assertEqual(self.rows(), before)

    def test_malformed_typed_flags_and_wrong_user_fail_closed(self):
        self.conn.execute(f"UPDATE {ICON} SET dispLocation='2'")
        self.conn.execute(f"UPDATE {CONCEPT} SET dispLocation='2'")
        before = self.rows()
        self.assertEqual(prep.repair_default_game_display(self.conn, USER, GAME), 0)
        with self.assertRaises(ValueError):
            prep.repair_default_game_display(self.conn, -1, GAME)
        self.assertEqual(self.rows(), before)


class ImportTests(unittest.TestCase):
    def test_explicit_store_plus_descriptors_and_rejection(self):
        with tempfile.TemporaryDirectory(prefix='retail-system-apps-') as temp:
            root = Path(temp)
            firmware, destination = root/'fw', root/'mms'
            for parent, title, uri, flags in (
                ('NPXS40071/appdb/default','NPXS40071','pslibrary:home',146),
                ('NPXS40071/appdb/NPXS40139','NPXS40139','pslibrary:media',148),
                ('NPXS40037/appdb/default','NPXS40037','psplus:browse',147),
                ('NPXS40047/appdb/default','NPXS40047','pshome:gamestore',146)):
                p = firmware/'system_ex_b/rnps/apps'/parent
                p.mkdir(parents=True)
                (p/'param.json').write_text(json.dumps(dict(titleId=title, displayLocation=flags,
                    applicationCategoryType=0x4000000, contentId='fixture-'+title, hubAppUri=uri,
                    localizedParameters={'defaultLanguage':'en-US','en-US':{'titleName':title}})))
                (p/'icon0.png').write_bytes(b'fixture')
            template = firmware/'system_b/priv/mms_ro/template_content.db'
            template.parent.mkdir(parents=True)
            template.write_bytes(b'fixture')
            with patch.object(prep, 'user_strings', return_value=['cicon.localConceptId metadata.localConceptId content.titleId icon.titleId']):
                self.assertEqual(prep.prepare(destination, root/'managed', firmware, USER, 'en-US',
                    system_apps=('NPXS40037','NPXS40047','NPXS40037')), 4)
                self.assertEqual(prep.prepare(destination, root/'managed', firmware, USER, 'en-US',
                    system_apps=('NPXS40037','NPXS40047')), 4)
                with self.assertRaises(ValueError):
                    prep.prepare(root/'invalid', root/'managed', firmware, USER, 'en-US', system_apps=('NPXS40008',))
            with closing(sqlite3.connect(destination/'app.db')) as conn:
                rows = conn.execute(f'SELECT titleId,dispLocation,hubAppUri FROM {ICON} WHERE titleId IN (?,?) ORDER BY titleId',
                                    ('NPXS40037','NPXS40047')).fetchall()
            self.assertEqual(rows, [('NPXS40037',147,'psplus:browse'), ('NPXS40047',146,'pshome:gamestore')])
            self.assertFalse((root/'invalid').exists())

    def test_actual_importer_preserves_media_rows_versions_and_marker(self):
        with tempfile.TemporaryDirectory(prefix='retail-game-import-', dir=Path.home()/'AppData/Local/Temp/opencode') as temp:
            root = Path(temp)
            firmware, external, destination = (root/n for n in ('fw','apps','mms'))
            for title, flags in (('default',146),('NPXS40139',148)):
                p = firmware/'system_ex_b/rnps/apps/NPXS40071/appdb'/title
                p.mkdir(parents=True)
                actual_title = 'NPXS40071' if title=='default' else title
                param = dict(titleId=actual_title, displayLocation=flags, applicationCategoryType=0x4000000,
                    contentId='fixture-'+actual_title, localizedParameters={'defaultLanguage':'en-US','en-US':{'titleName':actual_title}})
                (p/'param.json').write_text(json.dumps(param))
                (p/'icon0.png').write_bytes(b'fixture icon, not a usable image')
            for title, category in (('PPSA17221',0),('PPSA01616',65536)):
                p = external/title/'sce_sys'
                p.mkdir(parents=True)
                param = dict(titleId=title, applicationCategoryType=category, contentId='fixture-'+title,
                    localizedParameters={'defaultLanguage':'en-US','en-US':{'titleName':title}})
                if category:
                    param['displayLocation'] = 188
                (p/'param.json').write_text(json.dumps(param))
                (p/'icon0.png').write_bytes(b'fixture icon')
            template = firmware/'system_b/priv/mms_ro/template_content.db'
            template.parent.mkdir(parents=True)
            template.write_bytes(b'fixture copy-only template')
            destination.mkdir()
            marker = destination/'app.db-corrupt'
            marker.write_bytes(bytes.fromhex('0100e1fe'))
            with closing(sqlite3.connect(destination/'app.db')) as conn:
                conn.execute('PRAGMA user_version=123')
                conn.execute('CREATE TABLE custom_rows(id PRIMARY KEY, value)')
                conn.execute("INSERT INTO custom_rows VALUES(1,'keep')")
                conn.commit()
            strings = ['cicon.localConceptId metadata.localConceptId content.titleId icon.titleId']
            with patch.object(prep, 'user_strings', return_value=strings):
                self.assertEqual(prep.prepare(destination, root/'unused-managed', firmware, USER,'en-US',False,external),4)
                self.assertEqual(prep.prepare(destination, root/'unused-managed', firmware, USER,'en-US',False,external),4)
            with closing(sqlite3.connect(destination/'app.db')) as conn:
                self.assertEqual(conn.execute(f'SELECT dispLocation FROM {ICON} WHERE titleId=?',('PPSA17221',)).fetchone(), (138,))
                self.assertEqual(conn.execute(f'SELECT dispLocation FROM {ICON} WHERE titleId=?',('PPSA01616',)).fetchone(), (188,))
                self.assertEqual(conn.execute('SELECT * FROM custom_rows').fetchall(), [(1,'keep')])
                self.assertEqual(conn.execute('PRAGMA user_version').fetchone(), (123,))
                self.assertEqual(conn.execute('SELECT count(*) FROM tbl_contentinfo').fetchone(), (4,))
            self.assertEqual(marker.read_bytes(), bytes.fromhex('0100e1fe'))


if __name__ == '__main__':
    unittest.main()
