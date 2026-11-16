"""Prepare separate copied-store Home control/candidate trials; never launch.

The caller must stop writers to the source first. This copies ordinary files,
retains original reports and an unmodified candidate-side app.db outside the
guest tree, and excludes only reparse-point IPC endpoints (not normal files).
Neither candidate installation nor this tool certifies native recovery.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import stat


def digest(path):
    h = hashlib.sha256()
    with path.open('rb') as f:
        for block in iter(lambda: f.read(1024 * 1024), b''):
            h.update(block)
    return h.hexdigest()


def inventory(root, permit_ipc_endpoints):
    files, directories, skipped = {}, [], []
    def visit(directory):
        with os.scandir(directory) as entries:
            for entry in entries:
                path = Path(entry.path)
                rel = path.relative_to(root).as_posix()
                info = entry.stat(follow_symlinks=False)
                reparse = bool(getattr(info, 'st_file_attributes', 0) & 0x400) or stat.S_ISLNK(info.st_mode)
                if reparse:
                    if permit_ipc_endpoints and rel.startswith('ipc/'):
                        skipped.append(rel)
                        continue
                    raise ValueError(f'unsupported source link/reparse point: {rel}')
                if stat.S_ISDIR(info.st_mode):
                    directories.append(rel)
                    visit(path)
                elif stat.S_ISREG(info.st_mode):
                    files[rel] = digest(path)
                else:
                    raise ValueError(f'unsupported source object: {rel}')
    visit(root)
    return {'files': dict(sorted(files.items())), 'directories': sorted(directories), 'skipped_ipc_endpoints': sorted(skipped)}


def copy_inventory(source, destination, listing):
    destination.mkdir()
    for directory in listing['directories']:
        (destination / directory).mkdir(parents=True, exist_ok=True)
    for name, expected in listing['files'].items():
        target = destination / name
        target.parent.mkdir(parents=True, exist_ok=True)
        with target.open('xb') as out, (source / name).open('rb') as src:
            shutil.copyfileobj(src, out, 1024 * 1024)
        if digest(target) != expected:
            raise RuntimeError(f'copy/source mismatch: {name}')


USER_ROOTS = ('user/home', 'priv/home', 'eap/rodata/home')
USER_JOURNAL = 'priv/user/local-user-storage'
RECEIPT_NAME = re.compile(r'user-([0-9a-f]{8})\.receipt\Z')
RECEIPT_HEADER = re.compile(rb'KYTY-LOCAL-USER-1\n([0-9a-f]{8}) ([0-9a-f]{32}) ([PG]) ([CDU])\n')


def plain(path, directory):
    info = path.lstat()
    if (stat.S_ISLNK(info.st_mode) or getattr(info, 'st_file_attributes', 0) & 0x400 or
            not (stat.S_ISDIR(info.st_mode) if directory else stat.S_ISREG(info.st_mode))):
        raise ValueError(f'unsafe storage path: {path}')


def canonical_directory(path):
    # Match LocalUserStorage::Validate: no symlink/reparse ancestor before
    # canonicalization, then use generic UTF-8 spelling in std::quoted.
    current = path.absolute()
    while True:
        plain(current, True)
        if current.parent == current:
            break
        current = current.parent
    return path.resolve(strict=True).as_posix()


def quoted(path):
    return ('"' + path.replace('\\', '\\\\').replace('"', '\\"') + '"\n').encode('utf-8')


def plan_receipt_relocation(source, clone):
    """Validate copied source/clone ownership; return rewrites, without writing."""
    source_journal = source / USER_JOURNAL
    clone_journal = clone / USER_JOURNAL
    if not source_journal.exists():
        if clone_journal.exists():
            raise ValueError('clone journal differs from source')
        return []
    plain(source_journal, True)
    plain(clone_journal, True)
    names = sorted(p.name for p in source_journal.iterdir() if p.name.startswith('user-') and p.name.endswith('.receipt'))
    clone_names = sorted(p.name for p in clone_journal.iterdir() if p.name.startswith('user-') and p.name.endswith('.receipt'))
    if names != clone_names:
        raise ValueError('clone receipt set differs from source')
    if not names:
        return []
    source_layout = [canonical_directory(source / p) for p in (*USER_ROOTS, USER_JOURNAL)]
    clone_layout = [canonical_directory(clone / p) for p in (*USER_ROOTS, USER_JOURNAL)]
    source_tail = b''.join(quoted(p) for p in source_layout)
    clone_tail = b''.join(quoted(p) for p in clone_layout)
    plans = []
    for name in names:
        match_name = RECEIPT_NAME.fullmatch(name)
        if not match_name:
            raise ValueError(f'unsupported receipt name: {name}')
        original_path, clone_path = source_journal / name, clone_journal / name
        plain(original_path, False)
        plain(clone_path, False)
        raw = original_path.read_bytes()
        header = RECEIPT_HEADER.match(raw)
        if (len(raw) > 65536 or not header or header.group(1).decode() != match_name.group(1) or
                (header.group(4) == b'U' and header.group(3) != b'P') or
                raw != header.group(0) + source_tail or clone_path.read_bytes() != raw):
            raise ValueError(f'invalid, foreign, or changed receipt: {name}')
        user, token = header.group(1).decode(), header.group(2).decode()
        owner = b'KYTY-LOCAL-USER-1 ' + header.group(1) + b' ' + header.group(2) + b'\n'
        owned = 0
        for root in USER_ROOTS:
            for leaf in (user, f'.kyty-user-{user}-{token}.stage', f'.kyty-user-{user}-{token}.retired'):
                a, b = source / root / leaf, clone / root / leaf
                if a.exists() != b.exists():
                    raise ValueError(f'profile ownership layout differs: {root}/{leaf}')
                if a.exists():
                    plain(a, True); plain(b, True)
                    for marker in (a / '.kyty-local-user-owner', b / '.kyty-local-user-owner'):
                        plain(marker, False)
                        if marker.read_bytes() != owner:
                            raise ValueError(f'profile owner mismatch: {root}/{leaf}')
                    owned += 1
        if not owned:
            raise ValueError(f'no ownership marker proves receipt scope: {name}')
        plans.append((name, raw, header.group(0) + clone_tail))
    return plans


def apply_receipt_relocation(clone, archive, plans):
    if not plans:
        return {}
    archive.mkdir()
    result = {}
    for name, original, relocated in plans:
        with (archive / name).open('xb') as out:
            out.write(original)
        if (archive / name).read_bytes() != original:
            raise RuntimeError(f'archived receipt mismatch: {name}')
        path = clone / USER_JOURNAL / name
        if path.read_bytes() != original:
            raise ValueError(f'clone receipt changed before relocation: {name}')
        pending = path.with_name(path.name + '.clone-relocation')
        with pending.open('xb') as out:
            out.write(relocated)
            out.flush()
            os.fsync(out.fileno())
        os.replace(pending, path)  # Only an exclusively created clone is changed.
        result[f'{USER_JOURNAL}/{name}'] = {'original_sha256': hashlib.sha256(original).hexdigest(),
                                           'relocated_sha256': hashlib.sha256(relocated).hexdigest()}
    return result


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--source-store', type=Path, required=True)
    p.add_argument('--source-stage', type=Path, required=True)
    p.add_argument('--comparison', type=Path, required=True, help='completed compare_local_home_catalog.py output')
    p.add_argument('--output', type=Path, required=True, help='new directory, outside source trees')
    p.add_argument('--source-quiescent', action='store_true', help='confirm that no process is writing either source')
    a = p.parse_args()
    if not a.source_quiescent:
        p.error('caller must verify no source writers and provide --source-quiescent')
    store, stage, comparison = (x.resolve(strict=True) for x in (a.source_store, a.source_stage, a.comparison))
    output = a.output.resolve()
    if any(output == x or output in x.parents or x in output.parents for x in (store, stage, comparison)):
        raise ValueError('output overlaps a source or comparison')
    receipt = json.loads((comparison / 'input-manifest.json').read_text(encoding='utf-8'))
    result = json.loads((comparison / 'comparison-receipt.json').read_text(encoding='utf-8'))
    if 'comparison passed; no native recovery/admission or Home claim' not in (comparison / 'comparison.log').read_text(encoding='utf-8'):
        raise ValueError('comparison did not pass')
    candidate = comparison / 'comparison/candidate/app.db'
    candidate_marker = comparison / 'comparison/candidate/app.db-corrupt'
    candidate_hash = digest(candidate)
    if candidate_hash != result.get('candidate_app_db_sha256') or result.get('users') != receipt.get('users') or result.get('source_hashes_unchanged') is not True:
        raise ValueError('candidate differs from the completed comparison receipt')
    if digest(candidate_marker) != receipt['sha256']['app.db-corrupt']:
        raise ValueError('candidate marker differs from copied source')
    sources = inventory(store, True)
    stages = inventory(stage, False)
    for suffix in ('-wal', '-shm', '-journal'):
        if 'priv/mms/app.db' + suffix in sources['files']:
            raise ValueError('AppBrowse sidecars differ from the comparison snapshot')
    for source, key in [('priv/mms/app.db', 'app.db'), ('priv/mms/app.db-corrupt', 'app.db-corrupt'), ('priv/user/users.json', 'users.json')]:
        if sources['files'].get(source) != receipt['sha256'][key]:
            raise ValueError(f'current source differs from comparison: {source}')
    # The selected actual firmware must still supply the staged Library bytes.
    for key in receipt['sha256']:
        if key.startswith('library/') and digest(Path(receipt['source_paths'][key])) != receipt['sha256'][key]:
            raise ValueError(f'firmware source changed: {key}')
    output.mkdir(parents=False, exist_ok=False)
    for label in ('control', 'candidate'):
        target = output / label
        target.mkdir()
        copy_inventory(store, target / 'sd', sources)
        copy_inventory(stage, target / 'stage', stages)
    if inventory(store, True) != sources or inventory(stage, False) != stages or digest(candidate) != candidate_hash:
        raise RuntimeError('source changed while copying; retained trials must not be launched')
    plans = {label: plan_receipt_relocation(store, output / label / 'sd') for label in ('control', 'candidate')}
    relocated = {label: apply_receipt_relocation(output / label / 'sd', output / label / 'original-local-user-receipts', plans[label])
                 for label in ('control', 'candidate')}
    dest = output / 'candidate/sd/priv/mms/app.db'
    # Move only our newly copied file; never remove an existing source or marker.
    dest.rename(output / 'candidate/original-app.db')
    with dest.open('xb') as out, candidate.open('rb') as src:
        shutil.copyfileobj(src, out, 1024 * 1024)
    if digest(dest) != candidate_hash:
        raise RuntimeError('candidate copy mismatch')
    actual_control = inventory(output / 'control/sd', True)
    actual_candidate = inventory(output / 'candidate/sd', True)
    expected_control = dict(sources['files'])
    expected_candidate = dict(expected_control)
    for name, hashes in relocated['control'].items():
        expected_control[name] = hashes['relocated_sha256']
    for name, hashes in relocated['candidate'].items():
        expected_candidate[name] = hashes['relocated_sha256']
    expected_candidate['priv/mms/app.db'] = candidate_hash
    if actual_control['files'] != expected_control or actual_candidate['files'] != expected_candidate:
        raise RuntimeError('trial store differs beyond candidate app.db')
    if digest(output / 'candidate/original-app.db') != sources['files']['priv/mms/app.db']:
        raise RuntimeError('retained original app.db mismatch')
    if inventory(store, True) != sources or inventory(stage, False) != stages or digest(candidate) != candidate_hash:
        raise RuntimeError('source changed during final validation; retained trials must not be launched')
    (output / 'trial-manifest.json').write_text(json.dumps({
        'store': sources, 'stage': stages, 'candidate_app_db_sha256': candidate_hash,
        'receipt_relocation': relocated,
        'comparison': str(comparison), 'source_quiescence': 'caller asserted; not process-verified',
        'native_recovery': 'not established', 'runtime_home': 'not established'
    }, indent=2) + '\n', encoding='utf-8')
    print('Prepared isolated control and candidate; native recovery and Home remain unverified.')
    print('Copied regular files; excluded source IPC reparse endpoints:', len(sources['skipped_ipc_endpoints']))
    print('Output:', output)


if __name__ == '__main__':
    main()
