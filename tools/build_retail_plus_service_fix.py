"""Isolated two-object production overlay + scheduling-only regression fixture.

Never starts an emulator or edits a source/stage/store. Two compile jobs maximum,
serial links, scoped outputs and complete read-only base-input hash receipts.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess

BASE = '05d0ef41c5b3e68aaec8d5e45af7aeb6374c69a5af05da441f65e9da13f4be13'
SOURCES = ('src/libs/npNativeManagerEndpoint.cpp','src/loader/runtimeNpManagerClient.cpp',
           'tests/RetailNpCoordinatorFailureTests.cpp')


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--base',type=Path,required=True)
    p.add_argument('--output',type=Path,required=True)
    args=p.parse_args()
    base,out=args.base.resolve(),args.output.resolve()
    repo=Path(__file__).resolve().parents[1]
    sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
    assert sha(base/'kyty_emulator.exe')==BASE
    templates={n:(base/n).read_text() for n in ('kyty_emulator.exe.rsp','shutdown_regression.exe.rsp',
        'm2_regression.exe.rsp','autologin_regression.exe.rsp','theme_session_tests.exe.rsp')}
    inputs={base/'kyty_emulator.exe',base/'compile.rsp'}
    inputs.update(base/name for name in (*templates,'regressions.json','launch-build-receipt.json','libwinpthread-1.dll'))
    for text in templates.values():
        for line in text.splitlines():
            path=Path(line.strip('"'))
            if path.suffix.lower() in ('.obj','.lib') and path.is_file():inputs.add(path)
    pins={str(path):sha(path) for path in inputs}
    sources={s:sha(repo/s) for s in (*SOURCES,'src/libs/npNativeManagerEndpoint.h')}
    out.mkdir(exist_ok=False)
    shutil.copyfile(base/'compile.rsp',out/'compile.rsp')
    def compile(source):
        result=subprocess.run([r'C:/Program Files/LLVM/bin/clang-cl.exe','@'+str(out/'compile.rsp'),
            '/c',str(repo/source),'/Fo'+str(out/(Path(source).name+'.obj'))],capture_output=True,text=True)
        (out/(Path(source).name+'.log')).write_text(result.stdout+result.stderr)
        assert result.returncode==0,result.stdout+result.stderr
    with ThreadPoolExecutor(max_workers=2) as pool:list(pool.map(compile,SOURCES))
    def link(name,fixture=False,template=None):
        lines=[]
        text=templates[template or ('shutdown_regression.exe.rsp' if fixture else 'kyty_emulator.exe.rsp')]
        replaced=[]
        for line in text.splitlines():
            bare=line.strip('"')
            if bare.lower().startswith(('/out:','/pdb:','/map:','/lldmap:')):
                flag=bare.split(':',1)[0]
                suffix='.pdb' if flag.lower()=='/pdb' else '.map' if flag.lower() in ('/map','/lldmap') else '.exe'
                line='"'+flag+':'+str(out/(Path(name).stem+suffix))+'"'
            elif fixture and bare.endswith(('npNativeManagerEndpoint.cpp.obj','runtimeNpManagerClient.cpp.obj')):
                replaced.append(bare);continue
            elif fixture and bare.endswith('RetailShutdownStdioTests.cpp.obj'):
                line='"'+str(out/'RetailNpCoordinatorFailureTests.cpp.obj')+'"';replaced.append(bare)
            elif not fixture and bare.endswith(('npNativeManagerEndpoint.cpp.obj','runtimeNpManagerClient.cpp.obj')):
                line='"'+str(out/Path(bare).name)+'"';replaced.append(bare)
            lines.append(line)
        assert len(replaced)==(3 if fixture else 2)
        if fixture:lines.append('/opt:ref')
        rsp=out/(name+'.rsp');rsp.write_text('\n'.join(lines))
        run=subprocess.run([r'C:/Program Files/LLVM/bin/lld-link.exe','@'+str(rsp)],capture_output=True,text=True)
        (out/(name+'.link.log')).write_text(run.stdout+run.stderr)
        assert run.returncode==0,run.stdout+run.stderr
        # The fixture's first object overrides only Retained scheduling calls;
        # preserve the real factory/lifecycle exports needed by base objects.
        # This test-only /force:multiple template is NEVER used for the candidate.
        duplicates=re.findall(r'duplicate symbol: ([^\n]+)',run.stderr)
        if fixture:
            assert len(duplicates)==7 and all('NpNativeTransport::Retained::' in d for d in duplicates)
        else:
            if template in ('shutdown_regression.exe.rsp','theme_session_tests.exe.rsp','autologin_regression.exe.rsp'):
                assert len(duplicates)==1 and 'InvalidateMemory' in duplicates[0]
            else:assert not duplicates
        return out/name
    shutil.copyfile(base/'libwinpthread-1.dll',out/'libwinpthread-1.dll')
    fixture=link('np_coordinator_failure_tests.exe',True)
    results=[]
    for arguments in (['--old-policy'],[]):
        run=subprocess.run([str(fixture),*arguments],cwd=out,capture_output=True,text=True,timeout=30)
        results.append(dict(arguments=arguments,exit_code=run.returncode,stdout=run.stdout,stderr=run.stderr))
    (out/'tests.json').write_text(json.dumps(results,indent=2))
    assert results[0]['exit_code']==3 and 'terminal timeout was not propagated' in results[0]['stderr'],results
    assert results[1]['exit_code']==0,results
    regressions=[]
    theme=link('theme_session_regression.exe',template='theme_session_tests.exe.rsp')
    for flag in ('1',None,'0','true','11'):
        env=os.environ.copy();env.pop('KYTY_RETAIL_LOCAL_TEST_BOOT',None)
        env['KYTY_APPDB_CORE_AUTH_RECORD']='fixture-selection-only-core'
        env['KYTY_APPDB_UI_AUTH_RECORD']='fixture-selection-only-ui'
        if flag is not None:env['KYTY_RETAIL_LOCAL_TEST_BOOT']=flag
        arguments=[str(out/('fixture-theme-'+str(flag))),'on' if flag=='1' else 'off']
        run=subprocess.run([str(theme),*arguments],env=env,cwd=out,capture_output=True,text=True,timeout=60)
        regressions.append(dict(executable=theme.name,arguments=arguments,flag=flag,exit_code=run.returncode,stdout=run.stdout,stderr=run.stderr))
    prior=json.loads((base/'regressions.json').read_text())
    binaries={}
    for row in prior:
        name=row['executable']
        # The ancestor receipt uses the simple name; all outputs remain isolated.
        name=Path(name).name
        if name not in binaries:binaries[name]=link(name,template=name+'.rsp')
        env=os.environ.copy();env.pop('KYTY_RETAIL_LOCAL_TEST_BOOT',None)
        if row['flag'] is not None:env['KYTY_RETAIL_LOCAL_TEST_BOOT']=row['flag']
        run=subprocess.run([str(binaries[name]),*row['arguments']],env=env,cwd=out,capture_output=True,text=True,timeout=60)
        regressions.append(dict(executable=name,arguments=row['arguments'],flag=row['flag'],exit_code=run.returncode,stdout=run.stdout,stderr=run.stderr))
    (out/'regressions.json').write_text(json.dumps(regressions,indent=2))
    assert all(r['exit_code']==0 for r in regressions),regressions
    candidate=link('kyty_emulator.exe')
    assert all(sha(Path(n))==h for n,h in pins.items())
    assert all(sha(repo/n)==h for n,h in sources.items())
    receipt=dict(candidate=str(candidate),candidate_sha256=sha(candidate),base_sha256=BASE,
        production_objects=['npNativeManagerEndpoint.cpp.obj','runtimeNpManagerClient.cpp.obj'],
        sources=sources,read_only_link_inputs=pins,tests=results,regressions=regressions,emulator_launched=False,
        limitation='Scheduling-only retained transport fixture; no accepted native service, membership result, or Home acceptance')
    (out/'receipt.json').write_text(json.dumps(receipt,indent=2))
    seed=json.loads((base/'launch-build-receipt.json').read_text())['compatible_seed_candidate_sha256']
    launch=dict(candidate=str(candidate),candidate_sha256=sha(candidate),compatible_seed_candidate_sha256=seed,
        tests=[results[1],*regressions],full_receipt_sha256=sha(out/'receipt.json'),
        note='Expected old-policy negative control is preserved in the full receipt, not misreported as a passing run')
    (out/'launch-build-receipt.json').write_text(json.dumps(launch,indent=2))
    print(json.dumps({k:receipt[k] for k in ('candidate','candidate_sha256','tests')},indent=2))


if __name__=='__main__':main()
