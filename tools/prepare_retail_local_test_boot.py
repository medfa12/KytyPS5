"""Prepare a new LOCAL/TEST retail boot clone from one existing native profile.

No emulator launch. No new native user or authenticated identity is invented.
The setup-gate override is separately opt-in in the emulator environment.
"""
import argparse
import json
import os
from pathlib import Path
import shutil
import struct
import tempfile

from recover_retail_zero_profile import ancestors, content_only, digest, directory_manifest, manifest, write_new

USER_STORE = 'priv/user/users.json'
CANDIDATE_HASH = 'e611068dca9560150b092c73793da1e902ce56db1518c42a361e3da5b2dc8537'


def inspect_profile(source):
    home = source / 'priv/home'
    raw = (home / 'user_list.dat').read_bytes()
    if len(raw) != 72 or raw != (home / 'user_list.dat.0').read_bytes():
        raise ValueError('native lists must be matching 72-byte group-zero lists')
    words = struct.unpack('<18I', raw)
    user = words[0]
    if not (0x10010000 <= user <= 0x1fffffff) or words[1:16] != (0xffffffff,) * 15:
        raise ValueError('requires exactly one existing native home user')
    if words[16] < user or words[16] > 0x1fffffff or words[17] != 0:
        raise ValueError('unexpected high-water/group metadata')
    relative = f'priv/home/{user:08x}/config.dat'
    configs = {p.relative_to(source).as_posix() for p in home.rglob('config.dat*')}
    if configs != {relative}:
        raise ValueError('ambiguous profile configs/backups')
    data = (source / relative).read_bytes()
    if len(data) != 0x4000 or struct.unpack_from('<I', data)[0] != user or struct.unpack_from('<I', data, 0x58)[0] != 0x91e:
        raise ValueError('unsupported native profile schema or mismatched ID')
    name_field = data[4:21]
    if b'\0' not in name_field:
        raise ValueError('unterminated native profile name')
    name = name_field.split(b'\0', 1)[0].decode('utf-8')
    if not name or any(ord(c) < 32 for c in name):
        raise ValueError('invalid native profile name')
    if any(data[0x18:0x28]) or any(data[0x100:0x108]):
        raise ValueError('requires existing local user without passcode or bound account')
    auto_login, order = struct.unpack_from('<II', data, 0x48)
    if auto_login not in (0, 1) or order > 0x7fffffff:
        raise ValueError('unsupported native auto-login/order values')
    for prefix in ('user/home', 'priv/home', 'eap/rodata/home'):
        if not (source / prefix / f'{user:08x}').is_dir():
            raise ValueError('existing native profile provisioning roots required')
    companion = source / USER_STORE
    if companion.exists():
        if companion.stat().st_size > 65536:
            raise ValueError('unexpected companion user store size')
        existing = json.loads(companion.read_text(encoding='utf-8'))
        if existing != {'version': 1, 'next_user_number': 1, 'users': []}:
            raise ValueError('preserve populated/unknown companion user store')
    if (source / 'priv/user').exists():
        if any(p.is_file() and p != companion for p in (source / 'priv/user').rglob('*')):
            raise ValueError('ambiguous companion user-store files')
    flags = struct.unpack_from('<I', data, 0x5c)[0]
    changed = bytearray(data)
    struct.pack_into('<I', changed, 0x48, 1)
    struct.pack_into('<I', changed, 0x5c, flags & ~1)
    # These are existing HLE query views used by ShellUI, not a replacement for
    # native LoginMgr admission/login. Loading this JSON does not log a user in.
    mirror = {'version': 1, 'next_user_number': user - 0x10010000 + 2, 'users': [{
        'id': user, 'name': name, 'passcode': '', 'auto_login': True,
        'account_id': 0, 'signin_id': '', 'login_flag': flags & ~1,
        'user_status': 0, 'last_login_order': order}]}
    return user, relative, data, bytes(changed), (json.dumps(mirror, ensure_ascii=False, indent=2) + '\n').encode('utf-8')


def prepare(source, output, *, local_test_boot=False):
    if not local_test_boot:
        raise ValueError('explicit --local-test-boot is required')
    source, output = Path(source).absolute(), Path(output).absolute()
    ancestors(source)
    ancestors(output)
    source, output = source.resolve(), output.resolve()
    if not source.is_dir() or output.exists() or not output.parent.is_dir():
        raise ValueError('offline source and new output under an existing parent required')
    if source == output or source in output.parents or output in source.parents:
        raise ValueError('source and output must be separate trees')
    before, dirs = manifest(source), directory_manifest(source)
    user, profile, original, prepared, mirror = inspect_profile(source)
    stage = Path(tempfile.mkdtemp(prefix=output.name + '.incomplete-', dir=output.parent))
    try:
        copied = stage / 'sd'
        shutil.copytree(source, copied)
        if (manifest(source) != before or directory_manifest(source) != dirs
                or content_only(manifest(copied)) != content_only(before)
                or set(directory_manifest(copied)) != set(dirs)):
            raise ValueError('source changed or copy verification failed')
        if inspect_profile(copied) != (user, profile, original, prepared, mirror):
            raise ValueError('copied profile changed')
        write_new(stage / 'backup' / profile, original)
        if USER_STORE in before:
            write_new(stage / 'backup' / USER_STORE, (copied / USER_STORE).read_bytes())
        for name, data in ((profile, prepared), (USER_STORE, mirror)):
            temporary = copied / (name + '.localtest-new')
            write_new(temporary, data)
            os.replace(temporary, copied / name)
        after = manifest(copied)
        expected = content_only(before)
        expected[profile] = {'bytes': len(prepared), 'sha256': digest(prepared)}
        expected[USER_STORE] = {'bytes': len(mirror), 'sha256': digest(mirror)}
        if (content_only(after) != expected or manifest(source) != before
                or directory_manifest(source) != dirs):
            raise ValueError('preservation verification failed')
        receipt = {
            'schema': 1, 'mode': 'LOCAL/TEST saved-user startup, not completed OOBE or PSN',
            'user_id': f'0x{user:08x}', 'source': str(source), 'output': str(output),
            'tool_sha256': digest(Path(__file__).read_bytes()),
            'before': before, 'after': after, 'backup': manifest(stage / 'backup'),
            'source_unchanged_verified': True, 'guest_launched': False,
            'native_profile_changes': {'0x48': 'auto-login enabled', '0x5c': 'only incomplete bit0 cleared'},
            'native_lists_and_auth_files': 'byte-identical',
            'host_environment': {'KYTY_RETAIL_LOCAL_TEST_BOOT': '1', 'KYTY_FIRST_BOOT': '0'},
            'guest_environment': {'KYTY_USER_STORE': str(output / 'sd' / USER_STORE)},
            'candidate_sha256': CANDIDATE_HASH,
        }
        write_new(stage / 'receipt.json', (json.dumps(receipt, indent=2) + '\n').encode())
        if output.exists():
            raise ValueError('output appeared during preparation')
        stage.rename(output)
        return receipt
    except Exception:
        write_new(stage / 'FAILED.txt', b'Incomplete LOCAL/TEST preparation; do not launch.\n')
        raise


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--local-test-boot', action='store_true')
    args = parser.parse_args()
    receipt = prepare(args.source, args.output, local_test_boot=args.local_test_boot)
    print('Prepared LOCAL/TEST startup for existing native ID ' + receipt['user_id'] + '; see receipt.json.')
