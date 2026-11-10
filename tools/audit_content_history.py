"""Inventory reachable historical versions of release-review paths (metadata only)."""
import argparse
import hashlib
import json
import subprocess


def git(*args):
    return subprocess.check_output(['git', *args])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ref', default='HEAD')
    parser.add_argument('paths', nargs='+', help='Repository-relative literal paths or directories')
    args = parser.parse_args()
    revision = git('rev-parse', '--verify', args.ref + '^{commit}').decode().strip()
    paths = [':(literal)' + path for path in args.paths]
    current = {}
    for entry in git('ls-tree', '-rz', revision, '--', *args.paths).split(b'\0'):
        if entry:
            header, path = entry.split(b'\t', 1)
            mode, kind, oid = header.decode().split()
            if kind == 'blob':
                current[path.decode('utf-8')] = oid
    versions = {}
    # --raw reports both sides of changes, including deleted versions. -m
    # examines every merge parent; --no-renames retains both path identities.
    raw = git('log', revision, '--format=', '--raw', '-z', '-m',
              '--no-renames', '--no-abbrev', '--', *paths).split(b'\0')
    index = 0
    while index < len(raw):
        header = raw[index].strip()
        index += 1
        if not header:
            continue
        if not header.startswith(b':'):
            raise ValueError('Unexpected raw history record')
        oldmode, newmode, oldoid, newoid, status = header[1:].split()
        path = raw[index].decode('utf-8')
        index += 1
        for mode, oid in ((oldmode, oldoid), (newmode, newoid)):
            if mode.startswith(b'100') or mode == b'120000':
                versions.setdefault(oid.decode(), set()).add(path)
    rows = []
    for oid, names in sorted(versions.items()):
        data = git('cat-file', 'blob', oid)
        # Independently verify that each payload belongs to its reported object.
        assert hashlib.sha1(b'blob ' + str(len(data)).encode() + b'\0' + data).hexdigest() == oid
        rows.append({'blob': oid, 'bytes': len(data),
                     'sha256': hashlib.sha256(data).hexdigest(), 'paths': sorted(names),
                     'at_tip': any(current.get(name) == oid for name in names)})
    print(json.dumps({'revision': revision, 'requested_paths': args.paths,
                      'unique_blobs': len(rows),
                      'historical_only_blobs': sum(not row['at_tip'] for row in rows),
                      'unique_bytes': sum(row['bytes'] for row in rows),
                      'scope': 'Selected paths in reachable history; metadata inventory, not provenance clearance.',
                      'blobs': rows}, indent=2))


if __name__ == '__main__':
    main()
