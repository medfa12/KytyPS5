"""Device-free positive and loader-negative scenarios, each a fresh process.

Usage: python run_production_audiodec.py <exe> <firmware-root> <tones.eac3>
Outputs only bounded result summaries; full local fixture logs sit beside exe.
Set PATH to include the completed production build's dependency DLL directory.
"""
import pathlib
import shutil
import subprocess
import sys
import tempfile
from prepare_audiodec_importer import prepare

exe, firmware, tones = (pathlib.Path(p).resolve() for p in sys.argv[1:])
out = exe.parent
importer = out / 'importer.elf'
prepare(importer)
source = firmware / 'system_b/common/lib'

def stage(name, modules):
    root = pathlib.Path(tempfile.mkdtemp(prefix=name + '-', dir=out))
    lib = root / 'system_b/common/lib'
    lib.mkdir(parents=True, exist_ok=True)
    for module in modules:
        shutil.copyfile(source / module, lib / module)
    return root

def run(name, root, expected):
    log = out / (name + '.log')
    with log.open('wb') as stream:
        result = subprocess.run([str(exe), str(importer), str(root), str(tones)],
                                cwd=out, stdout=stream, stderr=subprocess.STDOUT, timeout=90)
    text = log.read_text(errors='replace')
    if expected is None:
        assert result.returncode == 0 and 'production integration passed' in text, (name, result.returncode, log)
        assert 'Required native DDP dependency unresolved' not in text
    else:
        assert result.returncode != 0 and expected in text, (name, result.returncode, log)
        assert 'native module startup returned' not in text and 'frame=' not in text
    print(f'{name}: PASS (exit={result.returncode}); {log.name}')

run('positive', firmware, None)
run('missing-dispatcher', stage('missing-dispatcher-root', []),
    'Required native audio decoder provider unavailable: libSceAudiodecCpu;')
run('wrong-prefix', stage('wrong-prefix-root', ['libSceAudiodecCpuDdp.sprx']),
    'Required native audio decoder provider unavailable: libSceAudiodecCpu;')
run('missing-ddp', stage('missing-ddp-root', ['libSceAudiodecCpu.sprx']),
    'Required native audio decoder provider unavailable: libSceAudiodecCpuDdp;')
broken = stage('missing-helper-root', ['libSceAudiodecCpu.sprx', 'libSceAudiodecCpuDdp.sprx'])
ddp = broken / 'system_b/common/lib/libSceAudiodecCpuDdp.sprx'
data = ddp.read_bytes()
assert data.count(b'aaDMGGkXFxo') == 1
ddp.write_bytes(data.replace(b'aaDMGGkXFxo', b'NoSuchFn123'))
run('missing-helper', broken, 'Required native DDP dependency unresolved: NoSuchFn123')
