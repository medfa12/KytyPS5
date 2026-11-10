"""Copy-only recovery for the exact evidenced retail zero-profile artifact.

Reads an offline system_data snapshot. Publishes OUTPUT/sd, byte-exact backups
outside that mount, and a receipt. Never edits SOURCE or an existing OUTPUT.
This is not a general profile repair tool. See the retail LoginMgr investigation.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import stat
import struct
import tempfile

LIST_HASH = '19d5349be7e2ef48130775ed03fb3fc36755ab32c7d0d0bec3cb00de1785bc66'
CONFIG_HASH = 'cdacaa2c4a5938d27be1da2d1c540df075849e61d7b559a786815c12dda7b6dc'
LIST = struct.pack('<18I', *([0] * 16 + [0x0fffffff, 0]))
EMPTY_LIST = struct.pack('<18I', *([0xffffffff] * 16 + [0x0fffffff, 0]))
CONFIG = bytearray(0x4000)
struct.pack_into('<I', CONFIG, 0x58, 0x91e)
CONFIG = bytes(CONFIG)
TARGETS = {
    'priv/home/user_list.dat': LIST,
    'priv/home/user_list.dat.0': LIST,
    'priv/home/00000000/config.dat': CONFIG,
}
ZERO_HOME = 'priv/home/00000000'
NO_BACKUP = ZERO_HOME + '/nobackup'


def digest(data):
    return hashlib.sha256(data).hexdigest()


def plain(path):
    info = path.lstat()
    if stat.S_ISLNK(info.st_mode) or getattr(info, 'st_file_attributes', 0) & 0x400:
        raise ValueError('links/reparse points are not accepted')
    if not (stat.S_ISDIR(info.st_mode) or stat.S_ISREG(info.st_mode)):
        raise ValueError('non-regular filesystem entry')
    return info


def ancestors(path):
    for entry in (path, *path.parents):
        if entry.exists() or entry.is_symlink():
            plain(entry)


def manifest(root):
    """Hash every regular file; retain metadata to detect concurrent writes."""
    result = {}
    for directory, dirs, files in os.walk(root, followlinks=False):
        plain(Path(directory))
        for name in dirs:
            plain(Path(directory) / name)
        for name in files:
            path = Path(directory) / name
            before = plain(path)
            h = hashlib.sha256()
            with path.open('rb') as stream:
                for block in iter(lambda: stream.read(1024 * 1024), b''):
                    h.update(block)
            after = plain(path)
            if (before.st_size, before.st_mtime_ns) != (after.st_size, after.st_mtime_ns):
                raise ValueError('source changed during read')
            result[path.relative_to(root).as_posix()] = {
                'bytes': after.st_size, 'sha256': h.hexdigest(), 'mtime_ns': after.st_mtime_ns}
    return result


def content_only(records):
    return {k: {f: v[f] for f in ('bytes', 'sha256')} for k, v in records.items()}


def directory_manifest(root):
    result = {}
    for directory, dirs, _ in os.walk(root, followlinks=False):
        for name in dirs:
            path = Path(directory) / name
            info = plain(path)
            result[path.relative_to(root).as_posix()] = {
                'mtime_ns': info.st_mtime_ns, 'ctime_ns': info.st_ctime_ns,
                'mode': info.st_mode, 'file_attributes': getattr(info, 'st_file_attributes', 0)}
    return result


def validate(root, records):
    if digest(LIST) != LIST_HASH or digest(CONFIG) != CONFIG_HASH:
        raise ValueError('internal artifact contract does not match evidence pins')
    expected = set(TARGETS)
    actual = {p for p in records if p.lower().startswith('priv/home/')}
    if actual != expected:
        raise ValueError('home inventory is not the exact three-file artifact; preserve ambiguous profiles')
    home = root / 'priv/home'
    # Native config save prepares this backup directory even when no backup
    # survives. Only this exact optional empty child is admitted.
    directories = {p.relative_to(home).as_posix() for p in home.rglob('*') if p.is_dir()}
    if directories not in ({'00000000'}, {'00000000', '00000000/nobackup'}):
        raise ValueError('unexpected home directory')
    nobackup = root / NO_BACKUP
    if nobackup.exists() and any(nobackup.iterdir()):
        raise ValueError('zero-profile nobackup must be empty')
    for name, expected_bytes in TARGETS.items():
        if records[name]['bytes'] != len(expected_bytes) or records[name]['sha256'] != digest(expected_bytes):
            raise ValueError('profile artifact hash/size mismatch')
        if (root / name).read_bytes() != expected_bytes:
            raise ValueError('profile artifact byte mismatch')
    # Native/local-HLE ownership is separate. Never migrate a populated or
    # unrecognized companion store, or a backup/temp copy of one.
    user_files = {p for p in records if p.lower().startswith('priv/user/')}
    if user_files - {'priv/user/users.json'}:
        raise ValueError('ambiguous companion user store files')
    if user_files:
        path = root / 'priv/user/users.json'
        if path.stat().st_size > 65536:
            raise ValueError('unexpected companion user store size')
        data = json.loads(path.read_text(encoding='utf-8'))
        if (not isinstance(data, dict) or set(data) != {'version', 'next_user_number', 'users'}
                or type(data['version']) is not int or data['version'] != 1 or data['users'] != []
                or type(data['next_user_number']) is not int or data['next_user_number'] != 1):
            raise ValueError('populated or unknown companion user store; preserve it')


def write_new(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open('xb') as stream:
        stream.write(data)
        stream.flush()
        os.fsync(stream.fileno())


def recover(source, output):
    source, output = Path(source).absolute(), Path(output).absolute()
    ancestors(source)
    ancestors(output)
    source, output = source.resolve(), output.resolve()
    if not source.is_dir() or output.exists():
        raise ValueError('source must be a directory and output must not exist')
    if source == output or source in output.parents or output in source.parents:
        raise ValueError('source and output must be separate trees')
    if not output.parent.is_dir():
        raise ValueError('output parent must exist')
    before = manifest(source)
    before_directories = directory_manifest(source)
    validate(source, before)
    stage = Path(tempfile.mkdtemp(prefix=output.name + '.incomplete-', dir=output.parent))
    try:
        copied = stage / 'sd'
        shutil.copytree(source, copied)
        if manifest(source) != before or directory_manifest(source) != before_directories:
            raise ValueError('source changed during copy')
        cloned = manifest(copied)
        if (content_only(cloned) != content_only(before)
                or set(directory_manifest(copied)) != set(before_directories)):
            raise ValueError('copy does not match source')
        validate(copied, cloned)
        for name, data in TARGETS.items():
            write_new(stage / 'backup' / name, data)
        quarantined_directories = [ZERO_HOME]
        if NO_BACKUP in before_directories:
            (stage / 'backup' / NO_BACKUP).mkdir()
            quarantined_directories.append(NO_BACKUP)
        # Preserve timestamps/mode where supported; record original and backup
        # metadata separately (creation times need not match on the new copy).
        for name in reversed(quarantined_directories):
            shutil.copystat(source / name, stage / 'backup' / name)
        for name in ('priv/home/user_list.dat', 'priv/home/user_list.dat.0'):
            # Only the tool-created, verified copy is modified.
            temporary = copied / (name + '.recovery-new')
            write_new(temporary, EMPTY_LIST)
            os.replace(temporary, copied / name)
        (copied / 'priv/home/00000000/config.dat').unlink()
        if NO_BACKUP in before_directories:
            (copied / NO_BACKUP).rmdir()
        (copied / 'priv/home/00000000').rmdir()
        after = manifest(copied)
        after_directories = directory_manifest(copied)
        expected = content_only(before)
        del expected['priv/home/00000000/config.dat']
        for name in ('priv/home/user_list.dat', 'priv/home/user_list.dat.0'):
            expected[name] = {'bytes': 72, 'sha256': digest(EMPTY_LIST)}
        if (content_only(after) != expected or manifest(source) != before
                or directory_manifest(source) != before_directories
                or set(after_directories) != set(before_directories) - set(quarantined_directories)):
            raise ValueError('post-recovery preservation verification failed')
        backups = manifest(stage / 'backup')
        backup_directories = directory_manifest(stage / 'backup')
        if content_only(backups) != {k: content_only(before)[k] for k in TARGETS}:
            raise ValueError('backup verification failed')
        if set(backup_directories) != {'priv', 'priv/home', *quarantined_directories}:
            raise ValueError('backup directory verification failed')
        receipt = {
            'schema': 1, 'operation': 'exact-retail-zero-profile-copy-recovery',
            'tool_sha256': digest(Path(__file__).read_bytes()),
            'source': str(source), 'output': str(output),
            'source_unchanged_verified': True, 'native_execution_verified': False,
            'before': before, 'after': after, 'backups': backups,
            'before_directories': before_directories, 'after_directories': after_directories,
            'backup_directories': backup_directories,
            'quarantined_directories': quarantined_directories,
            'changes': ['16 empty-ID sentinels in both lists; metadata retained',
                        'exact zero config quarantined outside sd'],
            'artifact_hashes': {'list': LIST_HASH, 'config': CONFIG_HASH},
            'required_candidate_sha256': 'bd540d9f06a558dcecfa8ef072935ab9a5c1cec1f05c09da9e92e7f8f63dafab',
        }
        write_new(stage / 'receipt.json', (json.dumps(receipt, indent=2) + '\n').encode())
        # rename refuses an existing destination on Windows; never replace a store.
        if output.exists():
            raise ValueError('output appeared during preparation')
        stage.rename(output)
        return receipt
    except Exception:
        # Preserve the failed staging tree for diagnosis; it has no published receipt contract.
        write_new(stage / 'FAILED.txt', b'Preparation failed; do not launch this incomplete store.\n')
        raise


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, required=True, help='offline system_data snapshot (read-only)')
    parser.add_argument('--output', type=Path, required=True, help='new recovery bundle; mount its sd child')
    args = parser.parse_args()
    recover(args.source, args.output)
    print('Verified copied-store recovery published; see output receipt.json. Native login remains unverified.')


if __name__ == '__main__':
    main()
