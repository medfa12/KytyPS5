"""Receipt-pinned absent-M2 overlay; two compile jobs, no shared writes or guest launches."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--base', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[1]
    base, out = args.base.resolve(), args.output.resolve()
    out.mkdir(exist_ok=False)
    sha = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
    receipt = json.loads((base/'receipt.json').read_text())
    assert receipt['candidate_sha256'] == 'e611068dca9560150b092c73793da1e902ce56db1518c42a361e3da5b2dc8537'
    pins = dict(receipt['read_only_link_inputs'])
    pins[str(base/'kyty_emulator.exe')] = receipt['candidate_sha256']
    response = (base/'kyty_emulator.exe.rsp').read_text()
    for line in response.splitlines():
        path = Path(line.strip('"'))
        if path.is_file():
            pins.setdefault(str(path), sha(path))
    def verify():
        for path, expected in pins.items():
            assert sha(Path(path)) == expected, path
    verify()
    sources = ['src/kernel/fileSystem.cpp', 'src/libs/libFsSandbox.cpp',
               'src/libs/libShellHits.cpp', 'tests/RetailM2AbsenceTests.cpp']
    options = (base/'compile.rsp').read_text()
    (out/'compile.rsp').write_text(options)
    def compile(source):
        obj = out/(Path(source).name + '.obj')
        result = subprocess.run([r'C:/Program Files/LLVM/bin/clang-cl.exe', '@'+str(out/'compile.rsp'),
            '/c', str(repo/source), '/Fo'+str(obj)], capture_output=True, text=True)
        (out/(Path(source).name + '.log')).write_text(result.stdout + result.stderr)
        assert result.returncode == 0, result.stdout + result.stderr
    with ThreadPoolExecutor(max_workers=2) as pool:
        list(pool.map(compile, sources))
    def link(name, test=False, old=False):
        lines = []
        for line in response.splitlines():
            bare = line.strip('"')
            if bare.lower().startswith(('/out:', '/pdb:', '/map:', '/lldmap:')):
                flag = bare.split(':', 1)[0]
                suffix = '.pdb' if flag.lower() == '/pdb' else '.map' if 'map' in flag.lower() else '.exe'
                line = '"' + flag + ':' + str(out/(Path(name).stem + suffix)) + '"'
            elif test and bare.endswith('main.cpp.obj'):
                line = '"' + str(out/'RetailM2AbsenceTests.cpp.obj') + '"'
            elif bare.endswith(('fileSystem.cpp.obj', 'libShellHits.cpp.obj')) or (
                    not old and bare.endswith('libFsSandbox.cpp.obj')):
                line = '"' + str(out/Path(bare).name) + '"'
            lines.append(line)
        rsp = out/(name+'.rsp')
        rsp.write_text('\n'.join(lines))
        result = subprocess.run([r'C:/Program Files/LLVM/bin/lld-link.exe', '@'+str(rsp)], capture_output=True, text=True)
        (out/(name+'.link.log')).write_text(result.stdout + result.stderr)
        assert result.returncode == 0 and 'duplicate symbol' not in result.stderr, result.stdout + result.stderr
        return out/name
    shutil.copyfile(base/'libwinpthread-1.dll', out/'libwinpthread-1.dll')
    control = link('m2_old_provider.exe', test=True, old=True)
    test = link('m2_tests.exe', test=True)
    results = []
    for exe in [control, test]:
        run = subprocess.run([str(exe)], cwd=out, capture_output=True, text=True, timeout=60)
        results.append(dict(executable=exe.name, exit_code=run.returncode, stdout=run.stdout, stderr=run.stderr))
    (out/'tests.json').write_text(json.dumps(results, indent=2))
    assert results[0]['exit_code'] != 0 and 'absent retail NVMe returns ENODEV' in results[0]['stderr'], results[0]
    assert results[1]['exit_code'] == 0, results[1]
    candidate = link('kyty_emulator.exe')
    verify()
    record = dict(base=str(base), base_sha256=sha(base/'kyty_emulator.exe'),
        candidate=str(candidate), candidate_sha256=sha(candidate), read_only_link_inputs=pins,
        sources={s: sha(repo/s) for s in sources + ['src/kernel/fileSystem.h', 'tests/RetailM2DeviceInfoTests.inc']},
        objects={Path(s).name+'.obj': sha(out/(Path(s).name+'.obj')) for s in sources},
        tests=results, guest_launched=False, limitation='Absent controller only; no attached storage or Home boot tested')
    (out/'receipt.json').write_text(json.dumps(record, indent=2))
    print(json.dumps({k: record[k] for k in ['candidate', 'candidate_sha256', 'tests']}, indent=2))


if __name__ == '__main__':
    main()
