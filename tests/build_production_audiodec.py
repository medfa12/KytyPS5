"""Create an isolated Ninja overlay from a completed production Ninja build.

Usage: python build_production_audiodec.py <production-build> <isolated-output>
Run ninja -C <isolated-output> -j2 afterwards. The production build must be idle.
Parent objects/archives are read only; this worktree's loader, libc and harness
are compiled afresh. No parent configuration, object, PDB or executable writes.
"""
import pathlib
import re
import sys

root = pathlib.Path(__file__).resolve().parent.parent
parent, out = (pathlib.Path(p).resolve() for p in sys.argv[1:])
out.mkdir(parents=True, exist_ok=True)
text = (parent / 'build.ninja').read_text()

def block(prefix):
    start = text.index(prefix)
    return text[start:text.index('\n\n', start)]

def field(section, name):
    return re.search(r'^  ' + name + r' = (.*)$', section, re.M)[1]

def quote(path):
    return '"' + str(path).replace('\\', '/') + '"'

def absolute(token):
    token = token.replace('$:', ':')
    return token if pathlib.Path(token).is_absolute() else (parent / token).as_posix()

compile_section = block('build CMakeFiles/kyty_emulator.dir/src/loader/runtimeLinker.cpp.obj:')
original_source = re.search(r' (\S+/src/loader/runtimeLinker.cpp) ', compile_section)[1].replace('$:', ':')
original_root = original_source.removesuffix('/src/loader/runtimeLinker.cpp')
includes = field(compile_section, 'INCLUDES').replace(original_root + '/src', root.as_posix() + '/src')
flags = field(compile_section, 'DEFINES') + ' ' + includes + ' ' + field(compile_section, 'FLAGS')
rules = ['rule compile\n  command = "C:/Program Files/LLVM/bin/clang-cl.exe" /nologo -TP ' + flags +
         ' /showIncludes /Fo$out -c -- $in\n  deps = msvc']
objects = []
for name, source in [('harness', 'tests/ProductionAudiodecTests.cpp'),
                     ('runtimeLinker', 'src/loader/runtimeLinker.cpp'), ('libC', 'src/libs/libC.cpp')]:
    rules.append('build ' + name + '.obj: compile ' + (root / source).as_posix().replace(':', '$:'))
    objects.append(name + '.obj')
link = block('build kyty_emulator.exe:')
for token in link.splitlines()[0].split(': ', 1)[1].split(' |', 1)[0].split()[1:]:
    if token.endswith(('/src/main.cpp.obj', '/src/loader/runtimeLinker.cpp.obj', '/src/libs/libC.cpp.obj')):
        continue
    objects.append(quote(absolute(token)))
libraries = [quote(absolute(t)) if '/' in t or '\\' in t else t for t in field(link, 'LINK_LIBRARIES').split()]
response = '/out:ProductionAudiodecTests.exe /machine:x64 /subsystem:console /DYNAMICBASE:NO\n'
response += '\n'.join(objects + libraries)
(out / 'link.rsp').write_text(response)
rules.append('rule link\n  command = "C:/Program Files/LLVM/bin/lld-link.exe" @link.rsp')
rules.append('build ProductionAudiodecTests.exe: link harness.obj runtimeLinker.obj libC.obj | link.rsp')
rules.append('default ProductionAudiodecTests.exe')
(out / 'build.ninja').write_text('\n\n'.join(rules) + '\n')
print(out)
