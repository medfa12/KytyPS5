"""Pinned e611 auto-login overlay + offline production stdio GNF fixture.

No emulator launch; at most two compiles, serial links. Parallel M.2 sources
are deliberately not compiled or included in this candidate.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess

BASE_HASH = 'e611068dca9560150b092c73793da1e902ce56db1518c42a361e3da5b2dc8537'
ASSETS = {
    'shutdown_ramp.gnf': (2304, '090515e14a8ab18ee9a2b33f7f0676887f4172306a4a9b529d4596cb0c31bcf7'),
    'Sce.Vsh.ShellUI.BGLayer.Particle0.gnf': (167936, 'e989bcd6325a7b889f869863e4c32019c89e7aaa4cace49795f17b6d4db23991'),
    'Sce.Vsh.ShellUI.BGLayer.Particle1.gnf': (167936, '9cb10598c0b00a99743ab504eb5a9026fdb18f326b182de31052c2acec1a782b'),
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--base', type=Path, required=True)
    parser.add_argument('--assets', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    base, assets, out = args.base.resolve(), args.assets.resolve(), args.output.resolve()
    repo = Path(__file__).resolve().parents[1]
    sha = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
    assert sha(base / 'kyty_emulator.exe') == BASE_HASH
    for name, (size, expected) in ASSETS.items():
        path = assets / name
        assert path.stat().st_size == size and sha(path) == expected, name
    responses = {name: (base / name).read_text() for name in ('kyty_emulator.exe.rsp', 'autologin_tests.exe.rsp')}
    inputs = {base / 'kyty_emulator.exe', base / 'compile.rsp', *(assets / n for n in ASSETS)}
    for text in responses.values():
        for line in text.splitlines():
            path = Path(line.strip('"'))
            if path.suffix.lower() in ('.obj', '.lib') and path.is_file():
                inputs.add(path)
    pins = {str(p): sha(p) for p in inputs}
    out.mkdir(exist_ok=False)
    shutil.copyfile(base / 'compile.rsp', out / 'compile.rsp')
    sources = ['src/libs/libC.cpp', 'src/libs/libGuestFile.cpp', 'tests/RetailShutdownStdioTests.cpp']
    def compile(source):
        result = subprocess.run([r'C:/Program Files/LLVM/bin/clang-cl.exe', '@' + str(out / 'compile.rsp'),
                                 '/c', str(repo / source), '/Fo' + str(out / (Path(source).name + '.obj'))],
                                capture_output=True, text=True)
        (out / (Path(source).name + '.log')).write_text(result.stdout + result.stderr)
        assert result.returncode == 0, result.stdout + result.stderr
    with ThreadPoolExecutor(max_workers=2) as pool:
        list(pool.map(compile, sources))
    def link(name, test=False, old=False):
        text = responses['autologin_tests.exe.rsp' if test else 'kyty_emulator.exe.rsp']
        lines = []
        for line in text.splitlines():
            bare = line.strip('"')
            if bare.lower().startswith(('/out:', '/pdb:', '/map:')):
                flag = bare.split(':', 1)[0]
                suffix = '.pdb' if flag.lower() == '/pdb' else '.exe.map' if flag.lower() == '/map' else '.exe'
                line = '"' + flag + ':' + str(out / (Path(name).stem + suffix)) + '"'
            elif bare.endswith('RetailLocalTestBootTests.cpp.obj'):
                line = '"' + str(out / 'RetailShutdownStdioTests.cpp.obj') + '"'
            elif not old and bare.endswith(('libC.cpp.obj', 'libGuestFile.cpp.obj')):
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
    old, new = link('shutdown_stdio_old.exe', True, True), link('shutdown_stdio_tests.exe', True)
    fixture = out / 'fixtures'
    fixture.mkdir()
    (fixture / 'short.bin').write_bytes(bytes([1, 2, 3, 4, 5]))
    results = []
    for exe in (old, new):
        run = subprocess.run([str(exe), str(assets), str(fixture)], cwd=out,
                             capture_output=True, text=True, timeout=60)
        results.append(dict(executable=exe.name, exit_code=run.returncode, stdout=run.stdout, stderr=run.stderr))
    (out / 'tests.json').write_text(json.dumps(results, indent=2))
    assert results[0]['exit_code'] != 0 and 'native GNF header fread' in results[0]['stderr'], results[0]
    assert results[1]['exit_code'] == 0, results[1]
    candidate = link('kyty_emulator.exe')
    assert all(sha(Path(p)) == h for p, h in pins.items())
    receipt = dict(candidate=str(candidate), candidate_sha256=sha(candidate), base_sha256=BASE_HASH,
                   recompiled_production_objects=['libC.cpp.obj', 'libGuestFile.cpp.obj'], read_only_link_inputs=pins,
                   sources={s: sha(repo / s) for s in sources + ['src/libs/libGuestFile.h']},
                   tests=results, guest_launched=False,
                   limitation='Production stdio/filesystem fixture; GPU invalidation replaced for host buffers; animation not rendered')
    (out / 'receipt.json').write_text(json.dumps(receipt, indent=2))
    print(json.dumps({k: receipt[k] for k in ('candidate', 'candidate_sha256', 'tests')}, indent=2))


if __name__ == '__main__':
    main()
