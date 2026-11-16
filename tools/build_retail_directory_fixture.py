"""Isolated Bond-candidate overlay and synthetic directory fixture; never launches guests."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--base', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--profile-registry', action='store_true', help='Run the native profile registry ABI fixture instead')
    parser.add_argument('--local-test-boot', action='store_true', help='Run local/test startup registry opt-in fixture')
    args = parser.parse_args()
    base, out = args.base.resolve(), args.output.resolve()
    out.mkdir(exist_ok=False)
    repo = Path(__file__).resolve().parents[1]
    sha = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
    receipt = json.loads((base/'receipt.json').read_text())
    pins = receipt['read_only_link_inputs']
    def verify():
        for path, expected in pins.items():
            assert sha(Path(path)) == expected, path
        assert sha(base/'kyty_emulator.exe') == '87cdf596b00141ab92ccd39f49f0ee2ccec9f88c5b6e0b1493bf06bfe88189cf'
    verify()
    fixture_source = 'NativeProfileRegistryTests.cpp' if args.profile_registry else 'GuestDirectoryTests.cpp'
    if args.local_test_boot:
        fixture_source = 'RetailLocalTestBootTests.cpp'
    sources = ['src/libs/libC.cpp', 'src/libs/libShellCoreFloor.cpp',
               'src/libs/guestDirectory.cpp', 'tests/' + fixture_source]
    options = (base/'libC.cpp.rsp').read_text()
    (out/'compile.rsp').write_text(options)
    def compile(source):
        obj = out/(Path(source).name + '.obj')
        result = subprocess.run([r'C:/Program Files/LLVM/bin/clang-cl.exe', '@'+str(out/'compile.rsp'),
            '/c', str(repo/source), '/Fo'+str(obj)], capture_output=True, text=True)
        (out/(Path(source).name + '.log')).write_text(result.stdout + result.stderr)
        if result.returncode:
            raise RuntimeError(source + ': ' + result.stdout + result.stderr)
    with ThreadPoolExecutor(max_workers=2) as pool:
        list(pool.map(compile, sources))
    def link(test, old=False):
        name = 'directory_control.exe' if old else 'directory_tests.exe' if test else 'kyty_emulator.exe'
        text = (base/('kernel_file_tests.exe.rsp' if test else 'kyty_emulator.exe.rsp')).read_text()
        lines = []
        for line in text.splitlines():
            bare = line.strip('"')
            if bare.lower().startswith(('/out:', '/pdb:', '/map:')):
                flag = bare.split(':', 1)[0]
                suffix = '.pdb' if flag.lower() == '/pdb' else '.exe.map' if flag.lower() == '/map' else '.exe'
                line = '"' + flag + ':' + str(out/(Path(name).stem + suffix)) + '"'
            elif bare.endswith('retail_devctl_tests.obj') and test:
                line = '"' + str(out/(fixture_source + '.obj')) + '"'
            elif not old and bare.endswith(('libC.cpp.obj', 'libShellCoreFloor.cpp.obj')):
                line = '"' + str(out/Path(bare).name) + '"'
            lines.append(line)
        lines.append('"' + str(out/'guestDirectory.cpp.obj') + '"')
        if test:
            lines.append('/force:multiple')  # Only the documented fixture renderer shim may duplicate.
        rsp = out/(name+'.rsp')
        rsp.write_text('\n'.join(lines))
        result = subprocess.run([r'C:/Program Files/LLVM/bin/lld-link.exe', '@'+str(rsp)], capture_output=True, text=True)
        (out/(name+'.link.log')).write_text(result.stdout + result.stderr)
        assert result.returncode == 0, result.stdout + result.stderr
        duplicates = re.findall(r'duplicate symbol: ([^\n]+)', result.stderr)
        assert (len(duplicates) == 1 and 'InvalidateMemory' in duplicates[0]) if test else not duplicates
        return out/name
    shutil.copyfile(base/'libwinpthread-1.dll', out/'libwinpthread-1.dll')
    control, test = link(True, True), link(True)
    results = []
    cases = [(control, 'control-root', None), (test, 'fixture-root', None)]
    if args.local_test_boot:
        cases = [(control, 'on', '1'), (test, 'on', '1'), (test, 'off', None),
                 (test, 'off', '0'), (test, 'off', 'true'), (test, 'off', '11')]
    for exe, fixture, flag in cases:
        environment = os.environ.copy()
        if args.local_test_boot:
            environment.pop('KYTY_RETAIL_LOCAL_TEST_BOOT', None)
            environment['KYTY_FIRST_BOOT'] = '1'
            if flag is not None:
                environment['KYTY_RETAIL_LOCAL_TEST_BOOT'] = flag
        run = subprocess.run([str(exe), fixture if args.local_test_boot else str(out/fixture)],
                             env=environment, cwd=out, capture_output=True, text=True, timeout=60)
        results.append(dict(executable=exe.name, flag=flag, exit_code=run.returncode, stdout=run.stdout, stderr=run.stderr))
    (out/'tests.json').write_text(json.dumps(results, indent=2))
    control_error = 'missing profile slot must be empty' if args.profile_registry else 'real home directory must open'
    if args.local_test_boot:
        control_error = 'local/test setup gate selection'
    assert results[0]['exit_code'] != 0 and control_error in results[0]['stderr'], results[0]
    assert all(result['exit_code'] == 0 for result in results[1:]), results[1:]
    candidate = link(False)
    verify()
    (out/'receipt.json').write_text(json.dumps(dict(base=str(base), base_sha256=sha(base/'kyty_emulator.exe'),
        candidate=str(candidate), candidate_sha256=sha(candidate),
        sources={s: sha(repo/s) for s in sources + ['src/libs/guestDirectory.h']},
        read_only_link_inputs=pins, tests=results, guest_launched=False,
        limitation='Fixture replaces GPU invalidation only; no native UserService execution or setup completion tested'), indent=2))
    print(json.dumps(dict(candidate=str(candidate), sha256=sha(candidate), tests=results), indent=2))


if __name__ == '__main__':
    main()
