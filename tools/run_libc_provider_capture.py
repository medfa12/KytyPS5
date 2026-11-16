"""One bounded ShellUI run with a PID/creation-bound provider inventory (Windows)."""
import ctypes as C
import argparse
from ctypes import wintypes as W
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import threading
import time
from local_paths import configured_path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--name', default='shinerngprovider1')
    parser.add_argument('--native-libc-startup', action='store_true')
    parser.add_argument('--firmware', type=Path, help='Extracted firmware root (or KYTY_FIRMWARE_ROOT)')
    args_in = parser.parse_args()
    firmware = configured_path(args_in.firmware)
    if not re.fullmatch(r'[A-Za-z0-9_-]{1,48}', args_in.name):
        parser.error('Invalid isolated run name')
    root = Path(__file__).resolve().parents[1]
    name = args_in.name
    run = root / '_Runs' / name
    pidfile = root / '_Logs' / (name + '.pid')
    output = Path(os.environ['LOCALAPPDATA']) / 'Temp/opencode'
    exe = root / '_Build/windows/kyty_emulator.exe'
    provider = firmware / 'system_b/common/lib/libSceLibcInternal.sprx'
    if run.exists() or pidfile.exists():
        raise RuntimeError('Refusing reused run identity')
    # The caller also checks for an existing emulator before launching this script.
    check = subprocess.run(['powershell.exe', '-NoProfile', '-Command',
                            'if (Get-Process kyty_emulator -ErrorAction SilentlyContinue) { exit 1 }'], timeout=10)
    if check.returncode:
        raise RuntimeError('Another emulator is running')
    shutil.copytree(root / '_SystemData', run / 'system-data', ignore=shutil.ignore_patterns('ipc'))
    metadata = dict(sha256=hashlib.sha256(exe.read_bytes()).hexdigest(), bytes=exe.stat().st_size)
    (run / 'executable.json').write_text(json.dumps(metadata, indent=2))
    print(metadata, flush=True)
    env = dict(os.environ, KYTY_IPMI_OBSERVE='1', PYTHONDONTWRITEBYTECODE='1')
    args = ['powershell.exe', '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File',
            str(root / 'tools/run_shellui_1360.ps1'), '-LogName', name,
            '-FirmwareRoot', str(firmware),
            '-SystemDataPath', str(run / 'system-data'), '-NoDatabases',
            '-PrintfDirection', 'File', '-NoGraphicsDebugDump', '-Visible',
            '-ForegroundUser', '-RnpsReplay', '-NativeNpHttpPool', '-Seconds', '30', '-MaxLogMB', '64']
    if args_in.native_libc_startup:
        args.append('-NativeLibcStartup')
    k = C.WinDLL('kernel32', use_last_error=True)
    k.OpenProcess.argtypes = [W.DWORD, W.BOOL, W.DWORD]
    k.OpenProcess.restype = W.HANDLE
    k.GetProcessTimes.argtypes = [W.HANDLE] + [C.POINTER(W.FILETIME)] * 4
    k.TerminateProcess.argtypes = [W.HANDLE, W.UINT]
    k.GetExitCodeProcess.argtypes = [W.HANDLE, C.POINTER(W.DWORD)]
    k.CloseHandle.argtypes = [W.HANDLE]
    stream = run / '_kyty.txt'
    streams = [stream, root / '_Logs' / (name + '.log'), root / '_Logs' / (name + '.stderr.log')]
    process = None
    stopped = threading.Event()
    watcher = None
    report = dict(stopping_threshold_bytes=32 * 1024 * 1024, stopping_threshold_seconds=25,
                  polling_interval_seconds=.05, stop_reason=None)
    failure = None
    with (run / 'launcher.log').open('w') as log:
        launcher = subprocess.Popen(args, cwd=root, env=env, stdout=log, stderr=subprocess.STDOUT)
        try:
            deadline = time.monotonic() + 40
            pid = None
            while launcher.poll() is None and time.monotonic() < deadline:
                try:
                    text = pidfile.read_text(encoding='utf-8-sig').strip()
                    if text.isdecimal() and int(text) > 0:
                        pid = int(text)
                        break
                except FileNotFoundError:
                    pass
                time.sleep(.05)
            if pid is None:
                raise RuntimeError('No current run PID')
            # Retain this exact process object for fallback cleanup, avoiding PID reuse.
            process = k.OpenProcess(0x1001, False, pid)
            if not process:
                raise C.WinError(C.get_last_error())
            times = [W.FILETIME() for _ in range(4)]
            if not k.GetProcessTimes(process, *(C.byref(t) for t in times)):
                raise C.WinError(C.get_last_error())
            creation = (times[0].dwHighDateTime << 32) | times[0].dwLowDateTime
            created_unix = creation / 10000000 - 11644473600
            kill_at = time.monotonic() + max(0, 25 - (time.time() - created_unix))

            def supervise():
                while not stopped.wait(.05):
                    size = sum(path.stat().st_size for path in streams if path.exists())
                    if size >= report['stopping_threshold_bytes'] or time.monotonic() >= kill_at:
                        report['stop_reason'] = 'log_threshold' if size >= report['stopping_threshold_bytes'] else 'time_threshold'
                        k.TerminateProcess(process, 1)
                        return

            watcher = threading.Thread(target=supervise, daemon=True)
            watcher.start()
            deadline = time.monotonic() + 8
            evidence = None
            while time.monotonic() < deadline and launcher.poll() is None:
                if stream.exists():
                    with stream.open('rb') as source:
                        raw = source.read(64 * 1024 * 1024 + 1)
                    if len(raw) > 64 * 1024 * 1024:
                        raise RuntimeError('Inventory log bound exceeded')
                    text = raw.decode('utf-8', errors='replace').replace('\\', '/')
                    places = re.findall(r'ps5-vsh: place libSceLibcInternal\.sprx base=(0x[0-9a-fA-F]+) mapped=(0x[0-9a-fA-F]+)', text)
                    expected = os.path.normcase(str(provider.resolve()))
                    path_lines = [path for path in re.findall(r'--- Relocate program: ([^\r\n]+?) ---', text)
                                  if os.path.normcase(str(Path(path).resolve())) == expected]
                    if len(places) == 1 and path_lines:
                        evidence = (raw, places[0], path_lines[:8])
                        break
                    if len(places) > 1:
                        raise RuntimeError('Ambiguous provider inventory')
                time.sleep(.25)
            if evidence is None:
                raise RuntimeError('Current provider placement/path evidence unavailable')
            raw, (base, mapped), paths = evidence
            inventory_log = run / 'inventory-log-prefix.txt'
            inventory_log.write_bytes(raw)
            inventory = dict(pid=pid, creation_time=creation, image_path=str(exe), provider_path=str(provider),
                             provider_bias=base, mapped_size=mapped,
                             placement_evidence=dict(log=str(inventory_log), sha256=hashlib.sha256(raw).hexdigest(),
                                                     full_path_lines=paths, base=base, mapped=mapped),
                             launcher_command=args)
            inventory_path = run / 'provider-inventory.json'
            inventory_path.write_text(json.dumps(inventory, indent=2))
            print(name, 'PID', pid, 'provider', base, flush=True)
            with (run / 'provider-capture.log').open('w') as capture:
                status = subprocess.run([sys.executable, '-B', str(root / 'tools/capture_libc_provider.py'),
                                         '--inventory', str(inventory_path), '--output', str(output / (name + '.json'))],
                                        cwd=root, env=env, stdout=capture, stderr=subprocess.STDOUT, timeout=20)
            print('capture exit', status.returncode, flush=True)
            if status.returncode:
                raise RuntimeError('Provider capture failed; see provider-capture.log')
            captured = json.loads((output / (name + '.json')).read_text())
            if captured.get('termination_reason') not in ('completed', 'process_exit') or not captured.get('samples'):
                raise RuntimeError('Capture produced no valid sampled evidence')
            if launcher.poll() is None:
                subprocess.run(['powershell.exe', '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File',
                                str(root / 'tools/capture_window.ps1'), '-ProcessId', str(pid),
                                '-Out', str(output / (name + '.png'))], cwd=root, timeout=10)
        except Exception as error:
            failure = error
        finally:
            if process and failure:
                k.TerminateProcess(process, 1)
            try:
                print('launcher exit', launcher.wait(timeout=35), flush=True)
            except subprocess.TimeoutExpired:
                if process:
                    k.TerminateProcess(process, 1)
                launcher.terminate()
                launcher.wait(timeout=10)
                failure = failure or RuntimeError('Launcher cleanup timeout')
            stopped.set()
            if watcher:
                watcher.join(timeout=2)
            if process:
                code = W.DWORD()
                if k.GetExitCodeProcess(process, C.byref(code)) and code.value == 259:
                    k.TerminateProcess(process, 1)
                    failure = failure or RuntimeError('Fallback emulator cleanup required')
                k.CloseHandle(process)
            report['final_log_bytes'] = {str(path): path.stat().st_size for path in streams if path.exists()}
            report['threshold_caveat'] = 'Polling stop thresholds, not synchronous byte/lifetime caps; measured sizes retained'
            if sum(report['final_log_bytes'].values()) > 64 * 1024 * 1024:
                failure = failure or RuntimeError('Measured log budget exceeded')
            (run / 'bounds.json').write_text(json.dumps(report, indent=2))
    print((run / 'launcher.log').read_text(errors='replace')[-450:], flush=True)
    if failure:
        raise failure


if __name__ == '__main__':
    main()
