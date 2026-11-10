"""Isolated UserService overlay on the five-object retail Home combined base."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess

BASE_HASH = '46ca14194ea0ec66c91e4e625664d990b3d0bef85cad2984f1bb81b096a4c6ea'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--base', type=Path, required=True)
    parser.add_argument('--compile-options', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    base, out = args.base.resolve(), args.output.resolve()
    repo = Path(__file__).resolve().parents[1]
    sha = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
    assert sha(base / 'kyty_emulator.exe') == BASE_HASH
    responses = {name: (base / name).read_text() for name in
                 ('kyty_emulator.exe.rsp', 'shutdown_stdio_tests.exe.rsp', 'm2_tests.exe.rsp', 'autologin_tests.exe.rsp')}
    inputs = {base / 'kyty_emulator.exe', args.compile_options.resolve()}
    for text in responses.values():
        for line in text.splitlines():
            path = Path(line.strip('"'))
            if path.suffix.lower() in ('.obj', '.lib') and path.is_file():
                inputs.add(path)
    pins = {str(p): sha(p) for p in inputs}
    out.mkdir(exist_ok=False)
    shutil.copyfile(args.compile_options, out / 'compile.rsp')
    sources = ['src/libs/libUserService.cpp', 'tests/RetailThemeSessionTests.cpp']
    source_pins = {s: sha(repo / s) for s in sources}
    def compile(source):
        result = subprocess.run([r'C:/Program Files/LLVM/bin/clang-cl.exe', '@' + str(out / 'compile.rsp'),
                                 '/c', str(repo / source), '/Fo' + str(out / (Path(source).name + '.obj'))],
                                capture_output=True, text=True)
        (out / (Path(source).name + '.log')).write_text(result.stdout + result.stderr)
        assert result.returncode == 0, result.stdout + result.stderr
    with ThreadPoolExecutor(max_workers=2) as pool:
        list(pool.map(compile, sources))
    def link(name, test=False, old=False, template=None, replace_fixture=True):
        text = responses[template or ('shutdown_stdio_tests.exe.rsp' if test else 'kyty_emulator.exe.rsp')]
        lines = []
        for line in text.splitlines():
            bare = line.strip('"')
            if bare.lower().startswith(('/out:', '/pdb:', '/map:')):
                flag = bare.split(':', 1)[0]
                suffix = '.pdb' if flag.lower() == '/pdb' else '.exe.map' if flag.lower() == '/map' else '.exe'
                line = '"' + flag + ':' + str(out / (Path(name).stem + suffix)) + '"'
            elif replace_fixture and bare.endswith('RetailShutdownStdioTests.cpp.obj'):
                line = '"' + str(out / 'RetailThemeSessionTests.cpp.obj') + '"'
            elif not old and bare.endswith('libUserService.cpp.obj'):
                line = '"' + str(out / 'libUserService.cpp.obj') + '"'
            lines.append(line)
        rsp = out / (name + '.rsp')
        rsp.write_text('\n'.join(lines))
        result = subprocess.run([r'C:/Program Files/LLVM/bin/lld-link.exe', '@' + str(rsp)], capture_output=True, text=True)
        (out / (name + '.link.log')).write_text(result.stdout + result.stderr)
        assert result.returncode == 0, result.stdout + result.stderr
        duplicates = re.findall(r'duplicate symbol: ([^\n]+)', result.stderr)
        host_buffer_fixture = test and template != 'm2_tests.exe.rsp'
        assert (len(duplicates) == 1 and 'InvalidateMemory' in duplicates[0]) if host_buffer_fixture else not duplicates
        return out / name
    shutil.copyfile(base / 'libwinpthread-1.dll', out / 'libwinpthread-1.dll')
    old, new = link('theme_session_old.exe', True, True), link('theme_session_tests.exe', True)
    cases = [(old, '1'), (new, '1'), (new, None), (new, '0'), (new, 'true'), (new, '11')]
    results = []
    for index, (exe, flag) in enumerate(cases):
        env = os.environ.copy()
        env.pop('KYTY_RETAIL_LOCAL_TEST_BOOT', None)
        # Selection-only markers, not source-bound credential fixtures.
        env['KYTY_APPDB_CORE_AUTH_RECORD'] = 'fixture-selection-only-core'
        env['KYTY_APPDB_UI_AUTH_RECORD'] = 'fixture-selection-only-ui'
        if flag is not None:
            env['KYTY_RETAIL_LOCAL_TEST_BOOT'] = flag
        run = subprocess.run([str(exe), str(out / ('fixture-' + str(index))), 'on' if flag == '1' else 'off'],
                             env=env, cwd=out, capture_output=True, text=True, timeout=60)
        results.append(dict(executable=exe.name, flag=flag, exit_code=run.returncode, stdout=run.stdout, stderr=run.stderr))
    (out / 'tests.json').write_text(json.dumps(results, indent=2))
    assert results[0]['exit_code'] != 0 and 'session exports must resolve' in results[0]['stderr'], results[0]
    assert all(r['exit_code'] == 0 for r in results[1:]), results[1:]
    # Relink, rather than merely rerun, all nine combined-base regressions with
    # the replacement UserService object and all five preceding fixes intact.
    shutdown = link('shutdown_regression.exe', True, template='shutdown_stdio_tests.exe.rsp', replace_fixture=False)
    m2 = link('m2_regression.exe', True, template='m2_tests.exe.rsp', replace_fixture=False)
    auto = link('autologin_regression.exe', True, template='autologin_tests.exe.rsp', replace_fixture=False)
    shutdown_args = json.loads((base / 'tests.json').read_text())[0]['arguments']
    regression_cases = [(shutdown, shutdown_args, None), (m2, [], None)]
    regression_cases += [(auto, ['on' if flag == '1' else 'off', user], flag) for flag, user in
                         [('1', '0x10010029'), ('1', '0x10010030'), ('1', '0x10000001'),
                          (None, '0x10010029'), ('0', '0x10010029'), ('true', '0x10010029'), ('11', '0x10010029')]]
    regressions = []
    for exe, arguments, flag in regression_cases:
        env = os.environ.copy()
        env.pop('KYTY_RETAIL_LOCAL_TEST_BOOT', None)
        if flag is not None:
            env['KYTY_RETAIL_LOCAL_TEST_BOOT'] = flag
        run = subprocess.run([str(exe), *arguments], env=env, cwd=out, capture_output=True, text=True, timeout=60)
        regressions.append(dict(executable=exe.name, arguments=arguments, flag=flag,
                                exit_code=run.returncode, stdout=run.stdout, stderr=run.stderr))
    (out / 'regressions.json').write_text(json.dumps(regressions, indent=2))
    assert all(r['exit_code'] == 0 for r in regressions), regressions
    candidate = link('kyty_emulator.exe')
    assert all(sha(Path(p)) == h for p, h in pins.items())
    assert all(sha(repo / s) == h for s, h in source_pins.items())
    receipt = dict(candidate=str(candidate), candidate_sha256=sha(candidate), base_sha256=BASE_HASH,
                   recompiled_production_objects=['libUserService.cpp.obj'], read_only_link_inputs=pins,
                   sources=source_pins, tests=results, regressions=regressions, guest_launched=False,
                   limitation='Synthetic retained boot-role session and callback fixture; no native Theme/Home execution')
    (out / 'receipt.json').write_text(json.dumps(receipt, indent=2))
    print(json.dumps({k: receipt[k] for k in ('candidate', 'candidate_sha256', 'tests')}, indent=2))


if __name__ == '__main__':
    main()
