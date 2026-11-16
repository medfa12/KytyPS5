"""Offline partial-schema feasibility probe, NEVER a database readiness producer.

Use copied app.db/profile inputs. Reads them read-only, clones SQLite to memory,
and runs only reviewed native DDL. No revisions, catalog rows, events, markers,
or disk database writes. Requires capstone via the pinned image reader.
"""
import argparse
import hashlib
import json
from pathlib import Path
import sqlite3

from inspect_vsh_db_contract_1360 import Image


def literal(image, address):
    return image.read(address, 512).split(b'\0', 1)[0].decode('ascii')


def digest_file(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def table_state(db):
    result = {}
    for (name,) in db.execute("SELECT name FROM sqlite_master WHERE type='table' ORDER BY name"):
        quoted = '"' + name.replace('"', '""') + '"'
        rows = sorted(repr(row) for row in db.execute('SELECT * FROM ' + quoted))
        columns = list(db.execute('PRAGMA table_info(' + quoted + ')'))
        result[name] = {'rows': len(rows), 'columns': columns,
                        'content_sha256': hashlib.sha256('\n'.join(rows).encode()).hexdigest()}
    return result


def probe(image_path, database, profiles):
    image = Image(image_path, 'ui')
    create_version = literal(image, 0xf6b2ac)
    prefix = literal(image, 0xfae119)
    suffix = literal(image, 0xf64c96)
    assert create_version == 'CREATE TABLE IF NOT EXISTS tbl_version(category TEXT NOT NULL,status INT,PRIMARY KEY(category));'
    assert prefix == 'CREATE TABLE IF NOT EXISTS '
    assert suffix == '(key TEXT NOT NULL,value TEXT,PRIMARY KEY(key));'
    # UI +54e948 copies tbl_info, +54e971 writes '_', +54e959 formats %010d.
    assert image.read(0xff5b00, 8) == b'tbl_info'
    assert literal(image, 0xfa1064) == '%010d'
    before_files = {str(p): digest_file(p) for p in (database, profiles)}
    users = [u['id'] for u in json.loads(profiles.read_text())['users']]
    if len(set(users)) != len(users) or any(type(u) is not int or not 0 <= u <= 0x7fffffff for u in users):
        raise ValueError('Unsupported/duplicate local user IDs')
    disk = sqlite3.connect(database.resolve().as_uri() + '?mode=ro', uri=True)
    memory = sqlite3.connect(':memory:')
    try:
        disk.execute('PRAGMA query_only=ON')
        integrity = [row[0] for row in disk.execute('PRAGMA integrity_check')]
        if integrity != ['ok']:
            raise ValueError('Copied input is not structurally valid: ' + repr(integrity))
        disk.backup(memory)
        original = table_state(memory)
        try:
            revision_before = list(memory.execute('SELECT status FROM tbl_version WHERE category=?', ('table_access_index',)))
        except sqlite3.DatabaseError as error:
            revision_before = {'error': str(error)}
        schema_before = list(memory.execute('SELECT * FROM sqlite_master ORDER BY name'))
        # A rolled-back real SQLite schema transaction must leave all old rows
        # and schema intact; this is also the failure/cancellation control.
        memory.execute('BEGIN IMMEDIATE')
        statements = [create_version] + [prefix + f'tbl_info_{u:010d}' + suffix for u in users]
        for sql in statements:
            memory.execute(sql)
        memory.rollback()
        assert table_state(memory) == original
        assert list(memory.execute('SELECT * FROM sqlite_master ORDER BY name')) == schema_before
        memory.execute('BEGIN IMMEDIATE')
        for sql in statements:
            memory.execute(sql)
        memory.commit()
        after = table_state(memory)
        assert all(after[name] == state for name, state in original.items()), 'Existing rows/schema changed'
        assert list(memory.execute('PRAGMA integrity_check')) == [('ok',)]
        revision_after = list(memory.execute('SELECT status FROM tbl_version WHERE category=?', ('table_access_index',)))
        report = {
            'scope': 'partial native DDL on memory clone only; NOT supported full schema/recovery',
            'firmware_sha256': image.digest, 'input_sha256': before_files,
            'integrity': integrity, 'local_user_ids': users,
            'original_tables': original,
            'created_tables': {n: s for n, s in after.items() if n not in original},
            'revision_before': revision_before, 'revision_after': revision_after,
            'rollback_preserved_schema_and_rows': True,
            'successful_fragment_preserved_existing_tables': True,
            'database_ready': False,
        }
    finally:
        memory.close()
        disk.close()
    assert before_files == {str(p): digest_file(p) for p in (database, profiles)}, 'Disk input changed'
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('original_ui', type=Path)
    parser.add_argument('copied_database', type=Path)
    parser.add_argument('copied_profiles', type=Path)
    args = parser.parse_args()
    print(json.dumps(probe(args.original_ui, args.copied_database, args.copied_profiles), indent=2))


if __name__ == '__main__':
    main()
