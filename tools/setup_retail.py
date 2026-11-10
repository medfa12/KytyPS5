"""Installed retail setup: validate inputs before writing isolated runtime data."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys

sys.dont_write_bytecode = True
from prepare_shellui_managed_1360 import LAUNCHER_PROFILES, validate_rnps_source
from extract_agc_register_data import extract as extract_agc


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--firmware', type=Path, required=True)
    parser.add_argument('--data', type=Path, required=True,
                        help='Dedicated writable directory outside firmware and installation')
    parser.add_argument('--url-config', type=Path)
    parser.add_argument('--rnps-source', type=Path)
    parser.add_argument('--core-record', type=Path)
    parser.add_argument('--ui-record', type=Path)
    parser.add_argument('--agc-module', type=Path,
                        help='Matching decrypted libSceAgc ELF (defaults to firmware/decrypted_modules/libSceAgc.sprx.elf)')
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument('--check', action='store_true', help='Check only; write nothing')
    mode.add_argument('--launch', action='store_true', help='Prepare and start the emulator')
    args = parser.parse_args()
    install = Path(__file__).resolve().parent.parent
    fw, data = args.firmware.resolve(), args.data.resolve()
    errors = []
    agc_path = data / 'agc-register-data.json'
    agc_bytes = None
    try:
        agc = extract_agc(args.agc_module or fw / 'decrypted_modules/libSceAgc.sprx.elf')
        agc_bytes = (json.dumps(agc, separators=(',', ':')) + '\n').encode('utf-8')
        if agc_path.exists() and agc_path.read_bytes() != agc_bytes:
            errors.append('Existing AGC data differs from matching firmware; use a fresh data directory')
    except (OSError, ValueError) as exc:
        errors.append('AGC prerequisite: provide --agc-module with the matching decrypted ELF: ' + str(exc))
    for protected in (fw, install):
        if data == protected or protected in data.parents or data in protected.parents:
            errors.append('Data directory must not overlap firmware or installation')
    if len(str(data / '_TempData/ipc/12345678')) + 24 >= 108:
        errors.append('Data path is too long for native IPC; choose a shorter directory')
    if not (install / 'kyty_emulator.exe').is_file():
        errors.append('Installed kyty_emulator.exe is missing')
    if importlib.util.find_spec('dnfile') is None:
        errors.append('Python dependency missing: install dnfile with python -m pip install dnfile')
    profile = None
    image = fw / 'system_ex_b/app/NPXS40087/eboot.bin'
    if not image.is_file():
        errors.append('Firmware is missing system_ex_b/app/NPXS40087/eboot.bin')
    else:
        digest = hashlib.sha256(image.read_bytes()).hexdigest()
        # Use the same detector as staging, rather than version-folder labels.
        detected = subprocess.run([sys.executable, '-B', str(install / 'tools/prepare_shellui_managed_1360.py'),
                                   '--fw-root', str(fw), '--detect-profile'], capture_output=True, text=True)
        if detected.returncode or detected.stdout.strip() not in LAUNCHER_PROFILES:
            errors.append('Unsupported firmware launcher SHA256: ' + digest)
        else:
            profile = detected.stdout.strip()
    for name in ('system_b', 'system_ex_b/vsh_asset'):
        if not (fw / name).is_dir():
            errors.append('Firmware directory missing: ' + name)
    if args.url_config is None or not args.url_config.is_file():
        errors.append('Provide --url-config with matching retail urlconfig_cache.json')
    else:
        try:
            if not isinstance(json.loads(args.url_config.read_text(encoding='utf-8-sig')), dict):
                raise ValueError('expected a JSON object')
        except (ValueError, OSError) as exc:
            errors.append('Invalid URL configuration: ' + str(exc))
    if args.rnps_source is None:
        errors.append('Provide --rnps-source with matching decrypted per-title RNPS overlays')
    else:
        try:
            validate_rnps_source(fw, data / 'managed', args.rnps_source.resolve())
        except (ValueError, OSError) as exc:
            errors.append(str(exc))
    if profile == 'alternate':
        for name, path, expected in (
            ('core', args.core_record, '5d9c1daacc67e91fbe1ddfdd0eac992270edc0e55986564cf579da89a95cc57a'),
            ('ui', args.ui_record, 'b35057cfe2d2d844f287325c438b39950ed6555f686592af546eb12fb950e0aa')):
            if path is None or not path.is_file():
                errors.append('Provide --' + name + '-record with the source-bound 136-byte admission record')
            elif path.stat().st_size != 136 or hashlib.sha256(path.read_bytes()).hexdigest() != expected:
                errors.append('Incorrect source-bound ' + name + ' record')
    if errors:
        print(json.dumps({'ready': False, 'profile': profile, 'errors': errors}, indent=2))
        return 1
    print(json.dumps({'ready': True, 'profile': profile, 'data': str(data),
                      'note': 'Input checks only; Home, app launch and PSN are not verified.'}, indent=2))
    if args.check:
        return 0
    data.mkdir(parents=True, exist_ok=True)
    if not agc_path.exists():
        with agc_path.open('xb') as output:
            output.write(agc_bytes)
    work = data / 'work'
    work.mkdir(exist_ok=True)
    command = ['powershell.exe', '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File',
               str(install / 'tools/run_shellui_1360.ps1'), '-FirmwareRoot', str(fw),
               '-RuntimeRoot', str(data), '-WorkDir', str(work),
               '-AgcRegisterData', str(agc_path),
               '-EmulatorPath', str(install / 'kyty_emulator.exe'),
               '-ManagedStage', str(data / 'managed'), '-SystemDataPath', str(data / 'system'),
               '-UrlConfigReference', str(args.url_config.resolve()),
               '-RnpsSource', str(args.rnps_source.resolve()), '-LauncherProfile', profile,
               '-LogName', 'retail', '-VshRuntime', '-ForegroundUser', '-UserStore',
               '-RnpsReplay', '-NoGraphicsDebugDump', '-MonoLogLevel', 'error']
    for option, path in (('-CoreAuthRecord', args.core_record), ('-UiAuthRecord', args.ui_record)):
        if path is not None:
            command += [option, str(path.resolve())]
    command += ['-Visible'] if args.launch else ['-PrepareOnly']
    # The runner's python invocations must use this interpreter, including venvs.
    env = os.environ.copy()
    # Optional developer references must not leak into a packaged fresh setup.
    for key in ('KYTY_NOTIFICATION_REFERENCE', 'KYTY_VSH_ASSET_PATH',
                'KYTY_APPDB_CORE_AUTH_RECORD', 'KYTY_APPDB_UI_AUTH_RECORD', 'KYTY_AGC_REGISTER_DATA'):
        env.pop(key, None)
    env['PATH'] = str(Path(sys.executable).parent) + os.pathsep + env.get('PATH', '')
    env['PYTHONDONTWRITEBYTECODE'] = '1'
    return subprocess.call(command, env=env, cwd=work)


if __name__ == '__main__':
    raise SystemExit(main())
