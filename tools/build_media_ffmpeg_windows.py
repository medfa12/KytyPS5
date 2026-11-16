"""Rebuild published 7.1.1 dependency with AC3/EAC3, isolated under this worktree."""
import hashlib
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tarfile
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
BASE = ROOT / '_Build/media-ffmpeg'
BUILD = BASE / 'build'
PREFIX = BASE / 'installed'
BUILD.mkdir(parents=True, exist_ok=True)
source = ROOT / '_TempData/ffmpeg-source-windows-x64.tar.gz'
if not (BASE / 'ffmpeg/configure').exists():
    source.parent.mkdir(parents=True, exist_ok=True)
    if not source.exists():
        urllib.request.urlretrieve('https://github.com/KytyPS5/ext-ffmpeg-core/releases/download/9ac4cfd195f1/ffmpeg-source-windows-x64.tar.gz', source)
    if hashlib.sha256(source.read_bytes()).hexdigest() != 'd7c4eef6a028295fc7154c9f9efbbef45cc2b7f0dcbc6d688bf90f4bfab41552':
        raise RuntimeError('Unexpected FFmpeg source package hash')
    with tarfile.open(source) as archive:
        archive.extractall(BASE, filter='data')
bat = os.environ.get('KYTY_VSDEVCMD', r'C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\Common7\Tools\VsDevCmd.bat')
output = subprocess.check_output(f'cmd.exe /d /s /c ""{bat}" -arch=x64 -host_arch=x64 >nul && set"')
env = os.environ.copy()
for line in output.decode(errors='replace').splitlines():
    if '=' in line:
        key, value = line.split('=', 1)
        if key:
            env[key] = value
env['PATH'] = 'C:/Program Files/LLVM/bin;C:/Strawberry/c/bin;C:/msys64/usr/bin;' + env.get('Path', env.get('PATH', ''))
env.pop('MSYS2_ARG_CONV_EXCL', None)
# Unpack make locally; do not install into or mutate the host MSYS installation.
make = BASE / 'make/usr/bin/make.exe'
if not (BASE / 'make-4.4.1-3-x86_64.pkg.tar.zst').exists():
    name = 'make-4.4.1-3-x86_64.pkg.tar.zst'
    archive = BASE / name
    urllib.request.urlretrieve('https://repo.msys2.org/msys/x86_64/' + name, archive)
    make.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run(['tar', '-xf', str(archive), '-C', str(BASE / 'make')], check=True)
for package, dll in [('libguile', 'msys-guile-3.0-1.dll'), ('libgc', 'msys-gc-1.dll')]:
    if not (make.parent / dll).exists() and not (Path('C:/msys64/usr/bin') / dll).exists():
        names = [dict(libguile='libguile-3.0.11-1-x86_64.pkg.tar.zst', libgc='libgc-8.2.8-1-x86_64.pkg.tar.zst')[package]]
        if not names:
            raise RuntimeError('Missing MSYS package: ' + package)
        archive = BASE / names[-1]
        urllib.request.urlretrieve('https://repo.msys2.org/msys/x86_64/' + names[-1], archive)
        subprocess.run(['tar', '-xf', str(archive), '-C', str(BASE / 'make')], check=True)

line = (BASE / 'configuration/x64-win-llvm-static-release-rel/ffbuild/config.log').open().readline()
options = shlex.split(line)[2:]
options = [x for x in options if not x.startswith(('--prefix=', '--extra-ldflags=', '--pkg-config=', '--ar='))]
options += ['--prefix=' + PREFIX.as_posix(), '--ar=llvm-ar.exe', '--pkg-config=false',
            '--enable-decoder=ac3', '--enable-decoder=eac3', '--enable-parser=ac3']
script = 'set -e\nexport PATH="' + make.parent.as_posix().replace('C:/', '/c/') + ':/c/Program Files/LLVM/bin:/c/Strawberry/c/bin:/usr/bin:$PATH"\n'
if not (BUILD / 'config.h').exists():
    script += shlex.join([(BASE / 'ffmpeg/configure').as_posix(), *options]) + '\n'
script += shlex.quote(make.as_posix()) + ' -j1\n'
script += shlex.quote(make.as_posix()) + ' -j1 install\n'
(BUILD / 'build.sh').write_text(script)
with (BASE / 'build.log').open('wb') as log:
    result = subprocess.run(['C:/msys64/usr/bin/bash.exe', 'build.sh'], cwd=BUILD, env=env, stdout=log, stderr=subprocess.STDOUT)
print('FFmpeg build exit', result.returncode, 'log', BASE / 'build.log')
if result.returncode == 0:
    share = PREFIX / 'share/ffmpeg'
    shutil.copytree(BASE / 'build-info', share, dirs_exist_ok=True)
    (share / 'MEDIA-BUILD.txt').write_text('Published 9ac4cfd195f1 FFmpeg 7.1.1 source; enabled ac3,eac3 decoders and ac3 parser.\n')
raise SystemExit(result.returncode)
