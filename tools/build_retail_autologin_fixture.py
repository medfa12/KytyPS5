"""Overlay only libC on the pinned local/test candidate; never launch guests."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess

BASE_HASH = '773dacca255a16cec22183899843977b413784ec54d3d41056fdc2bf86e2a0f6'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--base', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    base, out = args.base.resolve(), args.output.resolve()
    repo = Path(__file__).resolve().parents[1]
    sha = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
    assert sha(base / 'kyty_emulator.exe') == BASE_HASH
    responses = {name: (base / name).read_text() for name in ('kyty_emulator.exe.rsp', 'directory_tests.exe.rsp')}
    inputs = {base / 'kyty_emulator.exe', base / 'compile.rsp'}
    for text in responses.values():
        for line in text.splitlines():
            path = Path(line.strip('"'))
            if path.suffix.lower() in ('.obj', '.lib') and path.is_file():
                inputs.add(path)
    pins = {str(p): sha(p) for p in inputs}
    out.mkdir(exist_ok=False)
    shutil.copyfile(base / 'compile.rsp', out / 'compile.rsp')
    sources = ['src/libs/libC.cpp', 'tests/RetailLocalTestBootTests.cpp']
    def compile(source):
        result = subprocess.run([r'C:/Program Files/LLVM/bin/clang-cl.exe', '@' + str(out / 'compile.rsp'),
                                 '/c', str(repo / source), '/Fo' + str(out / (Path(source).name + '.obj'))],
                                capture_output=True, text=True)
        (out / (Path(source).name + '.log')).write_text(result.stdout + result.stderr)
        assert result.returncode == 0, result.stdout + result.stderr
    with ThreadPoolExecutor(max_workers=2) as pool:
        list(pool.map(compile, sources))
    def link(name, test=False, old=False):
        text = responses['directory_tests.exe.rsp' if test else 'kyty_emulator.exe.rsp']
        lines = []
        for line in text.splitlines():
            bare = line.strip('"')
            if bare.lower().startswith(('/out:', '/pdb:', '/map:')):
                flag = bare.split(':', 1)[0]
                suffix = '.pdb' if flag.lower() == '/pdb' else '.exe.map' if flag.lower() == '/map' else '.exe'
                line = '"' + flag + ':' + str(out / (Path(name).stem + suffix)) + '"'
            elif bare.endswith('RetailLocalTestBootTests.cpp.obj') or (not old and bare.endswith('libC.cpp.obj')):
                line = '"' + str(out / Path(bare).name) + '"'
            lines.append(line)
        rsp = out / (name + '.rsp')
        rsp.write_text('\n'.join(lines))
        result = subprocess.run([r'C:/Program Files/LLVM/bin/lld-link.exe', '@' + str(rsp)], capture_output=True, text=True)
        (out / (name + '.link.log')).write_text(result.stdout + result.stderr)
        assert result.returncode == 0, result.stdout + result.stderr
        duplicates = re.findall(r'duplicate symbol: ([^\n]+)', result.stderr)
        assert (len(duplicates) == 1 and 'InvalidateMemory' in duplicates[0]) if test else not duplicates
        return out / name
    shutil.copyfile(base / 'libwinpthread-1.dll', out / 'libwinpthread-1.dll')
    old, new = link('autologin_old.exe', True, True), link('autologin_tests.exe', True)
    cases = [(old, '1', '0x10010029'), (new, '1', '0x10010029'), (new, '1', '0x10010030'),
             (new, '1', '0x10000001'), (new, None, '0x10010029'), (new, '0', '0x10010029'),
             (new, 'true', '0x10010029'), (new, '11', '0x10010029')]
    results = []
    for exe, flag, user in cases:
        env = os.environ.copy()
        env.pop('KYTY_RETAIL_LOCAL_TEST_BOOT', None)
        env['KYTY_FIRST_BOOT'] = '1'
        if flag is not None:
            env['KYTY_RETAIL_LOCAL_TEST_BOOT'] = flag
        run = subprocess.run([str(exe), 'on' if flag == '1' else 'off', user], env=env, cwd=out,
                             capture_output=True, text=True, timeout=60)
        results.append(dict(executable=exe.name, flag=flag, user=user, exit_code=run.returncode,
                            stdout=run.stdout, stderr=run.stderr))
    (out / 'tests.json').write_text(json.dumps(results, indent=2))
    assert results[0]['exit_code'] != 0 and 'native auto-login selector' in results[0]['stderr'], results[0]
    assert all(r['exit_code'] == 0 for r in results[1:]), results[1:]
    candidate = link('kyty_emulator.exe')
    assert all(sha(Path(p)) == h for p, h in pins.items())
    receipt = dict(candidate=str(candidate), candidate_sha256=sha(candidate), base_sha256=BASE_HASH,
                   recompiled_production_objects=['libC.cpp.obj'], read_only_link_inputs=pins,
                   sources={s: sha(repo / s) for s in sources}, tests=results, guest_launched=False,
                   limitation='Registry ABI fixtures only; native auto-login/Home not executed')
    (out / 'receipt.json').write_text(json.dumps(receipt, indent=2))
    print(json.dumps({k: receipt[k] for k in ('candidate', 'candidate_sha256', 'tests')}, indent=2))


if __name__ == '__main__':
    main()
