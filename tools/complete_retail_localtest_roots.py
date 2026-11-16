"""Complete missing empty profile roots on a new verified LOCAL/TEST copy."""
import argparse
import json
from pathlib import Path
import shutil
import struct

from recover_retail_zero_profile import ancestors, content_only, digest, directory_manifest, manifest, write_new


def prepare(source_receipt, output):
    source_receipt, output = Path(source_receipt).absolute(), Path(output).absolute()
    ancestors(source_receipt)
    ancestors(output)
    if not source_receipt.is_file() or output.exists() or not output.parent.is_dir():
        raise ValueError('Existing receipt and new destination required')
    original = json.loads(source_receipt.read_text(encoding='utf-8'))
    if original.get('mode') != 'LOCAL/TEST saved-user startup, not completed OOBE or PSN' or not original.get('source_unchanged_verified'):
        raise ValueError('Unsupported local/test receipt')
    source = Path(original['output']).resolve() / 'sd'
    output = output.resolve()
    if not source.is_dir() or source == output or source in output.parents or output in source.parents:
        raise ValueError('Source and destination must be separate trees')
    ancestors(source)
    before, dirs = manifest(source), directory_manifest(source)
    if content_only(before) != content_only(original['after']):
        raise ValueError('Prepared source differs from its receipt')
    user = int(original['user_id'], 0)
    if not 0x10010000 <= user < 0x20000000:
        raise ValueError('Invalid existing native user')
    sd = source / 'priv/home'
    roster = (sd / 'user_list.dat').read_bytes()
    if len(roster) != 72 or roster != (sd / 'user_list.dat.0').read_bytes():
        raise ValueError('Native user roster mismatch')
    slots = struct.unpack('<18I', roster)
    if slots[0] != user or slots[1:16] != (0xffffffff,) * 15:
        raise ValueError('Native roster is not the expected single user')
    profile = sd / f'{user:08x}/config.dat'
    if len(profile.read_bytes()) != 0x4000 or struct.unpack_from('<I', profile.read_bytes())[0] != user:
        raise ValueError('Native profile identity mismatch')
    users = json.loads((source / 'priv/user/users.json').read_text(encoding='utf-8'))
    if len(users.get('users', [])) != 1 or users['users'][0]['id'] != user:
        raise ValueError('Companion user store mismatch')
    roots = [f'user/home/{user:08x}', f'priv/home/{user:08x}', f'eap/rodata/home/{user:08x}']
    if not (source / roots[1]).is_dir():
        raise ValueError('Existing native profile root is required')
    for rel in roots:
        p = source / rel
        if p.exists() and not p.is_dir():
            raise ValueError('Profile path is not a directory')
    output.mkdir()
    copied = output / 'sd'
    shutil.copytree(source, copied)
    if manifest(source) != before or directory_manifest(source) != dirs or content_only(manifest(copied)) != content_only(before):
        raise ValueError('Source changed or file copy failed')
    if set(directory_manifest(copied)) != set(dirs):
        raise ValueError('Directory copy failed')
    created = []
    for rel in roots:
        target = copied / rel
        if not target.exists():
            target.mkdir(parents=True)
            created.append(rel)
    if content_only(manifest(copied)) != content_only(before) or manifest(source) != before or directory_manifest(source) != dirs:
        raise ValueError('Unexpected content change')
    result = dict(original, output=str(output), source_unchanged_verified=True,
                  source_receipt=str(source_receipt), source_receipt_sha256=digest(source_receipt.read_bytes()),
                  after=manifest(copied), completed_empty_roots=created,
                  note='Copy-only LOCAL/TEST provisioning; no native login or PSN authentication asserted')
    write_new(output / 'receipt.json', (json.dumps(result, indent=2) + '\n').encode())
    return result


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-receipt', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    result = prepare(args.source_receipt, args.output)
    print(json.dumps({'user': result['user_id'], 'created_roots': result['completed_empty_roots'],
                      'receipt': str(args.output / 'receipt.json')}, indent=2))
