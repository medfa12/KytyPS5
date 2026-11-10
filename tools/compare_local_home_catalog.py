"""Copy a quiescent AppBrowse input and compare real-roster Library projections.

This is an offline diagnostic. It never launches a guest, modifies a source, or
asserts that the private candidate is safe for native recovery or activation.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys


def digest(path):
    h = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            h.update(block)
    return h.hexdigest()


def sidecars(path):
    return [str(path) + suffix for suffix in ('-wal', '-shm', '-journal')
            if Path(str(path) + suffix).exists()]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--app-db', type=Path, required=True)
    parser.add_argument('--roster', type=Path, required=True)
    parser.add_argument('--library', type=Path, required=True)
    parser.add_argument('--executable', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True, help='new, exclusively claimed diagnostic directory')
    args = parser.parse_args()
    app = args.app_db.resolve(strict=True)
    roster = args.roster.resolve(strict=True)
    library = args.library.resolve(strict=True)
    exe = args.executable.resolve(strict=True)
    marker = app.with_name(app.name + '-corrupt').resolve(strict=True)
    inputs = {'app.db': app, 'app.db-corrupt': marker, 'users.json': roster}
    for name in ('default', 'NPXS40139'):
        for file in ('param.json', 'icon0.png'):
            inputs[f'library/{name}/{file}'] = (library / name / file).resolve(strict=True)
    if app.name != 'app.db' or sidecars(app):
        raise ValueError('requires app.db with no live SQLite sidecars; do not copy an active journal/WAL')
    # The output must not be within an input tree or alias any source.
    output = args.output.resolve()
    protected_dirs = (app.parent, roster.parent, library)
    if (any(output == p or output in p.parents or p in output.parents for p in inputs.values()) or
            any(output == p or p in output.parents for p in protected_dirs)):
        raise ValueError('output overlaps an input')
    before = {name: digest(path) for name, path in inputs.items()}
    output.mkdir(parents=False, exist_ok=False)
    for name, path in inputs.items():
        target = output / 'input' / name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(path, target)
        if digest(target) != before[name]:
            raise RuntimeError(f'copy changed during snapshot: {name}')
    if sidecars(app) or any(digest(path) != before[name] for name, path in inputs.items()):
        raise RuntimeError('source changed during snapshot; private copy retained but not used')
    data = json.loads((output / 'input/users.json').read_text(encoding='utf-8'))
    users = [item['id'] for item in data['users']]
    if data.get('version') != 1 or not users or len(users) > 16 or len(set(users)) != len(users) or any(type(u) is not int or not 0 <= u <= 0x7fffffff for u in users):
        raise ValueError('invalid/empty saved roster')
    manifest = {'sha256': before, 'users': users, 'sqlite_sidecars': [], 'source_paths': {k: str(v) for k, v in inputs.items()}}
    (output / 'input-manifest.json').write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8')
    command = [str(exe), str(output / 'input/app.db'), str(output / 'input/library'), str(output / 'comparison'), *map(str, users)]
    result = subprocess.run(command, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, check=False)
    (output / 'comparison.log').write_text(result.stdout, encoding='utf-8')
    print(result.stdout, end='')
    after = {name: digest(path) for name, path in inputs.items()}
    unchanged = before == after and not sidecars(app)
    print('source hashes and no-sidecar state unchanged:', unchanged)
    print('diagnostic directory:', output)
    if not unchanged or result.returncode:
        raise SystemExit(result.returncode or 1)
    candidate = output / 'comparison/candidate/app.db'
    (output / 'comparison-receipt.json').write_text(json.dumps({
        'candidate_app_db_sha256': digest(candidate), 'users': users,
        'source_hashes_unchanged': True, 'native_recovery': 'not established'
    }, indent=2) + '\n', encoding='utf-8')


if __name__ == '__main__':
    sys.dont_write_bytecode = True
    main()
