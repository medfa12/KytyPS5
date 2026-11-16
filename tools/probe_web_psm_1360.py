"""Stage and launch an isolated regular 13.60 native web PSM boot probe."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import runpy
import shutil
import subprocess
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('firmware', 'common', 'title', 'run', 'executable', 'urlconfig'):
        parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--rnps-replay-dir', type=Path, help='Opt in to existing recorded RNPS plaintexts')
    options = parser.parse_args()
    firmware, common, title, run, executable = (
        getattr(options, name).resolve() for name in ('firmware', 'common', 'title', 'run', 'executable'))
    launcher = firmware / 'system_ex_b/app/NPXS40001'
    modules = [firmware / 'system_b/common/lib/libSceJemspace.sprx', common / 'libScePsm.sprx',
               common / 'libmonosgen-2.0.sprx', common / 'libScePsmUtil.sprx', common / 'mscorlib.dll.sprx',
               launcher / 'psm/Application/app.exe.sprx', firmware / 'system_b/common/lib/libSceLibcInternal.sprx',
               firmware / 'system_ex_b/common_ex/lib/libSceVshPng.sprx']
    urlconfig = options.urlconfig.resolve()
    if options.rnps_replay_dir and not options.rnps_replay_dir.is_dir():
        parser.error(f'Missing RNPS replay directory: {options.rnps_replay_dir}')
    for path in [executable, urlconfig, launcher / 'eboot.bin', title / 'sce_sys/param.json', *modules]:
        if not path.is_file():
            parser.error(f'Missing input: {path}')
    if run.exists():
        parser.error(f'Refusing to overwrite a prior probe: {run}')
    run.mkdir(parents=True)
    for name in ('data', 'sd', 'user', 'ipc', 'Application'):
        (run / name).mkdir()
    cache = run / 'sd/priv/cache/urlconfig/urlconfig_cache.json'
    cache.parent.mkdir(parents=True)
    shutil.copyfile(urlconfig, cache)
    stager = runpy.run_path(str(Path(__file__).with_name('prepare_shellui_managed_1360.py')))
    stager['prepare'](launcher / 'psm/Application', run / 'Application')
    stager['link_or_copy'](common / 'mscorlib.dll.sprx', run / 'Application/mscorlib.dll.sprx')
    args = [str(executable), '--game', str(launcher / 'eboot.bin'), '--param-json', str(title / 'sce_sys/param.json'),
            '--fw-root', str(firmware), '--network-connected', '--user-id', '268435457',
            '--screen-width', '1920', '--screen-height', '1080', '--window-width', '1280', '--window-height', '720',
            '--printf-direction', 'File', '--printf-output-file', str(run / 'guest.log')]
    mounts = [('/app0', title), ('/web_app_launcher', launcher), ('/app0/psm', launcher / 'psm'),
              ('/app0/psm/Application', run / 'Application'), ('/web_app_launcher/psm/Application', run / 'Application'),
              ('/common_ex/lib', common), ('/system_ex/common_ex/lib', common), ('/kyty/common_ex/lib', common),
              ('/system', firmware / 'system_b'), ('/system_ex', firmware / 'system_ex_b'),
              ('/data', run / 'data'), ('/system_data', run / 'sd'), ('/user', run / 'user'), ('/system_tmp', run / 'ipc')]
    for guest, host in mounts:
        args += ['--mount', f'{guest}={host}']
    for module in modules:
        args += ['--co-elf', str(module)]
    for setting in ('MONO_THREADS_SUSPEND=preemptive', 'MONO_LOG_LEVEL=warning'):
        args += ['--guest-env', setting]
    args += ['--guest-exec', '/web_app_launcher/eboot.bin']
    env = os.environ.copy()
    settings = dict(KYTY_NETWORK_HOST='1', KYTY_NATIVE_SSL='1', KYTY_NATIVE_LIBC_STARTUP='1', KYTY_MEDIA_TRACE='1')
    if options.rnps_replay_dir:
        settings.update(KYTY_RNPS_REPLAY='1', KYTY_RNPS_REPLAY_DIR=str(options.rnps_replay_dir.resolve()))
    env.update(settings)
    record = dict(executable=str(executable), arguments=args[1:], environment=settings,
                  urlconfig_source=str(urlconfig), urlconfig_sha256=hashlib.sha256(cache.read_bytes()).hexdigest(),
                  inherited_kyty_environment={key: value for key, value in env.items() if key.startswith('KYTY_')},
                  executable_sha256=hashlib.sha256(executable.read_bytes()).hexdigest())
    with (run / 'host.log').open('wb') as output:
        process = subprocess.Popen(args, cwd=run, env=env, stdout=output, stderr=subprocess.STDOUT)
    record.update(pid=process.pid, launched_at=time.time())
    (run / 'launch.json').write_text(json.dumps(record, indent=2), encoding='utf-8')
    print(json.dumps(dict(pid=process.pid, directory=str(run))))


if __name__ == '__main__':
    main()
