"""Read-only localhome8 catalog audit; offline rows never imply native visibility.

No launch, attach, store recovery, SQLite mutation, version stamping, or marker
removal. Only a new private JSON report is written when explicitly requested.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import sqlite3
import struct

USER = 0x10010029
CANDIDATE = '16d9d8c01bafbf6b6027be909637622811f284971a2ce879efdcc15631e46abd'
PINS = {
    'host.log': 'bedcfe186c0294267c0d51c172885b15bc86214f90051c48c74ea0d4646cbe28',
    'guest.log': 'dd484a9d7cea7a56747aa653b72df4e1873d79c4038e21e1b0d4ba79c27e3963',
    'window.png': '44711b6746b17e68a13644732e363f1033f8d4119165578e0061299312a9bd88',
}
EXTERNAL = {'PPSA01616', 'PPSA01651', 'PPSA01667', 'PPSA01783', 'PPSA05688', 'PPSA17221'}


def require(value, message):
    if not value:
        raise ValueError(message)


def digest(data):
    return hashlib.sha256(data).hexdigest()


def verify_rows(root, rows):
    for name, row in rows.items():
        path = root / name
        require(path.resolve().is_relative_to(root.resolve()) and not path.is_symlink(), 'Unsafe receipt path')
        data = path.read_bytes()
        require(len(data) == row['bytes'] and digest(data) == row['sha256'], f'Frozen file changed: {name}')


def native_evidence(guest):
    patterns = {
        'init_ver_check_100': 'InitClient DbAccessor::Init() : ret=0, ver_check_result=0x100',
        'corruption_rejected': 'Daemon::InitClient() error checkCorruption() err',
        'icon_query_failed': 'ExecQuerySQL(tbl_iconinfo) error 809c0103',
        'concept_query_failed': 'ExecQuerySQL(tbl_concepticoninfo) error 809c0103',
        'appinfo_connection_null': 'BeginTransaction() : m_connection is Null.',
        'browse_transaction_failed': 'BeginTransaction error 0x809c0103',
    }
    evidence = {key: dict(count=0, first_lines=[]) for key in patterns}
    later_reads = []
    for number, line in enumerate(guest.split('\n'), 1):
        if re.search(r'Read \d+ bytes from: .*[/\\]app\.db$', line.strip()):
            if len(later_reads) < 4:
                later_reads.append(dict(line=number, text=line.strip()))
        for key, text in patterns.items():
            if text in line:
                row = evidence[key]
                row['count'] += 1
                if len(row['first_lines']) < 4:
                    row['first_lines'].append(dict(line=number, text=line.strip()))
    rejected = all(evidence[k]['count'] for k in ('init_ver_check_100', 'corruption_rejected',
                                                  'icon_query_failed', 'concept_query_failed'))
    return dict(status='native_catalog_initialization_rejected' if rejected else 'not_established',
                evidence=evidence, later_app_db_reads=later_reads, native_visibility_verified=False,
                limitation='An initial native admission failure does not prove no later reader opened app.db.')


def display_predicates(row):
    """Individual predicates only, not complete queries or native delivery.

    Retail Accessor.Db 7f7dee85 metadata: SPACE_GAME=1<<SPACE_SHIFT(1),
    LIBRARY_DISPLAY=1<<LIBRARY_SHIFT(3), EXSW_DISPLAY=1<<EXSW_SHIFT(7).
    """
    flags = row['dispLocation']
    require(isinstance(flags, int), 'Noninteger display flags require separate review')
    visible = row['visible'] == 1
    return dict(game_space=visible and bool(flags & 2), library=visible and bool(flags & 8),
                home_game_switcher=visible and bool(flags & 2) and bool(flags & 128),
                predicate_only_not_full_query=True)


def roster_user(data):
    require(len(data) == 72, 'Unexpected native roster length')
    slots = struct.unpack('<18I', data)[:16]
    result, ended = [], False
    for user in slots:
        if user == 0xffffffff:
            ended = True
        else:
            require(not ended and 0x10010000 <= user < 0x20000000 and user not in result, 'Invalid native roster')
            result.append(user)
    require(USER in result, 'Saved native user absent')
    return result


def offline_database(path, user=USER):
    require(user == USER, 'This audit is scoped to saved native user 0x10010029')
    for suffix in ('-wal', '-journal'):
        sidecar = path.with_name(path.name + suffix)
        require(not sidecar.exists() or sidecar.stat().st_size == 0, 'Non-atomic SQLite sidecar needs separate review')
    before = digest(path.read_bytes())
    conn = sqlite3.connect(path.resolve().as_uri() + '?mode=ro&immutable=1', uri=True)
    try:
        conn.row_factory = sqlite3.Row
        integrity = [row[0] for row in conn.execute('PRAGMA integrity_check')]
        require(integrity == ['ok'], 'Frozen app.db fails SQLite integrity')
        tables = {row[0] for row in conn.execute("SELECT name FROM sqlite_master WHERE type='table'")}
        names = ('tbl_contentinfo', 'tbl_conceptmetadata', f'tbl_iconinfo_{user:010d}', f'tbl_concepticoninfo_{user:010d}')
        require(set(names) <= tables, 'Saved-user catalog tables missing')
        counts = {name: conn.execute(f'SELECT count(*) FROM "{name}"').fetchone()[0] for name in names}
        icon, concept = names[2:]
        rows = [dict(row) for row in conn.execute(f'''
            SELECT i.titleId, i.titleName, i.localConceptId, i.installStatus AS iconInstallStatus,
                   i.visible, i.dispLocation, i.platform, c.installStatus AS contentInstallStatus,
                   c.categoryType, c.metaDataPath, c.pprDeeplinkUri,
                   ci.validFlag, ci.installedStatus AS conceptInstalledStatus
            FROM "{icon}" AS i
            LEFT JOIN tbl_contentinfo AS c USING(titleId)
            LEFT JOIN "{concept}" AS ci USING(localConceptId)
            ORDER BY i.titleId''')]
        # A narrow diagnostic join, NOT the native query or its admission path.
        local_ids = [row[0] for row in conn.execute(f'''
            SELECT cicon.localConceptId FROM "{concept}" AS cicon
            LEFT JOIN tbl_conceptmetadata AS metadata USING(localConceptId)
            LEFT JOIN "{icon}" AS ticon USING(localConceptId)
            LEFT JOIN tbl_contentinfo AS content USING(titleId)
            WHERE cicon.validFlag=1 GROUP BY cicon.localConceptId LIMIT 100 OFFSET 0''')]
        for row in rows:
            row['display_predicates'] = display_predicates(row)
        return dict(database_sha256=before, integrity=integrity,
                    user_version=conn.execute('PRAGMA user_version').fetchone()[0],
                    table_counts=counts, rows=rows, diagnostic_join_ids=local_ids,
                    offline_only=True, native_visibility_verified=False)
    finally:
        conn.close()
        require(digest(path.read_bytes()) == before, 'Read-only database audit changed source')


def audit(checkpoint):
    require(checkpoint.name == 'checkpoint1' and checkpoint.parent.name == 'retail-localhome8', 'Only frozen localhome8 is authorized here')
    receipt = json.loads((checkpoint / 'receipt.json').read_bytes())
    verify_rows(checkpoint, receipt['files'])
    for name, pin in PINS.items():
        require(receipt['files'][name]['sha256'] == pin, 'Wrong frozen prefix')
    verify_rows(checkpoint / 'sd', receipt['store'])
    launch = json.loads((checkpoint / 'launch.json').read_bytes())
    require(launch['executable_sha256'] == CANDIDATE, 'Wrong executable candidate')
    arguments = launch['arguments']
    require(arguments[arguments.index('--user-id') + 1] == str(USER), 'Wrong launch user')
    require(set(launch['external_applications']['titles']) == EXTERNAL, 'Unexpected external app set')
    mounts = [arguments[i + 1] for i, v in enumerate(arguments[:-1]) if v == '--mount']
    title_mounts = [v for v in mounts if v.startswith('/system_ex/rnps/apps/')]
    require(len(title_mounts) == 19, 'Expected 19 counterpart title mounts')
    sd = checkpoint / 'sd'
    users = roster_user((sd / 'priv/home/user_list.dat').read_bytes())
    database = offline_database(sd / 'priv/mms/app.db')
    marker = sd / 'priv/mms/app.db-corrupt'
    marker_data = marker.read_bytes() if marker.exists() else b''
    require(len(marker_data) in (0, 4), 'Unexpected corruption marker format')
    native = native_evidence((checkpoint / 'guest.log').read_text(encoding='utf-8', errors='replace'))
    verify_rows(checkpoint / 'sd', receipt['store'])
    return dict(mode='READ ONLY frozen non-atomic checkpoint audit', user_id=hex(USER),
        candidate_sha256=CANDIDATE, checkpoint_receipt_sha256=digest((checkpoint / 'receipt.json').read_bytes()),
        frozen_files_verified=len(receipt['files']), frozen_store_files_verified=len(receipt['store']),
        roster_users=[hex(u) for u in users], title_mount_count=len(title_mounts), external_titles=sorted(EXTERNAL),
        database=database, corruption_marker_hex=marker_data.hex(), native=native,
        limitation='Integrity and offline rows do not prove native schema admission, query delivery, rendered tiles or launchability. Non-atomic copy is not a coherent live snapshot.',
        prohibited_shortcuts=['No guessed version/schema stamp', 'No corruption marker deletion',
                              'No fabricated native query results, authentication or entitlement', 'No live process access or store mutation'])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--checkpoint', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    output = args.output.resolve()
    approved = Path.home() / 'AppData/Local/Temp/opencode'
    require(output.parent == approved.resolve() and not output.exists(), 'Output must be a new private temp-root JSON file')
    report = audit(args.checkpoint.resolve())
    with output.open('x', encoding='utf-8') as stream:
        json.dump(report, stream, indent=2)
        stream.write('\n')
    print(json.dumps(dict(output=str(output), sha256=digest(output.read_bytes()),
                          native_status=report['native']['status'],
                          table_counts=report['database']['table_counts'],
                          native_visibility_verified=False), indent=2))


if __name__ == '__main__':
    main()
