"""Prepare, never run, a one-line barrier-only copy of the reviewed parent launcher.

The parent launcher deliberately removes inherited KYTY_* variables. A shell
environment assignment alone therefore cannot arm the pre-guest barrier.
The source launcher is not edited. Keep its __file__ directory unchanged.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re

SOURCE_SHA256 = 'a1f2a5ffc01692bcac33e40ca0d1f05894dd79185ce8ee493b215049283f4bbf'
ANCHOR = "env = {k:v for k,v in os.environ.items() if not k.startswith('KYTY_') and k not in ('SDL_AUDIO_DRIVER','SDL_AUDIODRIVER')}"


def transform(data, token):
    if not re.fullmatch('[0-9a-f]{32}', token):
        raise ValueError('token must be 32 lowercase hex characters')
    if hashlib.sha256(data).hexdigest() != SOURCE_SHA256:
        raise ValueError('parent launcher changed; review rather than weaken this pin')
    lines = data.decode('utf-8').splitlines(keepends=True)
    found = [i for i, line in enumerate(lines) if line.rstrip('\r\n') == ANCHOR]
    if len(found) != 1:
        raise ValueError('expected exactly one reviewed environment filter')
    index = found[0]
    newline = '\r\n' if lines[index].endswith('\r\n') else '\n'
    lines.insert(index + 1, "env['KYTY_PRE_GUEST_GATE_TOKEN'] = '" + token + "'" + newline)
    result = ''.join(lines).encode('utf-8')
    compile(result, '<barrier-only-launcher-copy>', 'exec')  # parse only
    return result


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--source', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--token', required=True)
    args = p.parse_args()
    source, output = args.source.resolve(), args.output.resolve()
    if source == output or source.parent != output.parent:
        p.error('output must be a NEW sibling of the source; __file__ controls stage paths')
    receipt = output.with_suffix('.receipt.json')
    if output.exists() or receipt.exists():
        p.error('refusing to overwrite a retained launcher/receipt')
    before = source.read_bytes()
    result = transform(before, args.token)
    with output.open('xb') as file:
        file.write(result)
    assert source.read_bytes() == before and output.read_bytes() == result
    report = dict(source=str(source), source_sha256=SOURCE_SHA256, output=str(output),
                  output_sha256=hashlib.sha256(result).hexdigest(), token=args.token,
                  change='only add host pre-guest gate token immediately after environment filtering',
                  launched=False, original_unchanged=True)
    with receipt.open('x', encoding='utf-8') as file:
        json.dump(report, file, indent=2)
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
