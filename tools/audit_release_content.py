"""Inventory a committed tree; findings are review candidates, not legal conclusions."""
import argparse
import hashlib
import json
import re
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ref', default='HEAD')
    args = parser.parse_args()
    revision = subprocess.check_output(['git', 'rev-parse', '--verify', args.ref + '^{commit}'], text=True).strip()
    entries = subprocess.check_output(['git', 'ls-tree', '-rz', revision]).split(b'\0')
    records = []
    for entry in entries:
        if entry:
            header, path = entry.split(b'\t', 1)
            mode, kind, oid = header.decode().split()
            records.append((mode, kind, oid, path.decode('utf-8')))
    blobs = [r for r in records if r[1] == 'blob']
    output = subprocess.check_output(['git', 'cat-file', '--batch'], input=''.join(r[2] + '\n' for r in blobs).encode())
    cursor = 0
    binaries, references, arrays = [], [], []
    total = 0
    reference = re.compile(r'(?i)\bsdkc\b|\bsdk\s*(?:14|13)?[^\n]{0,60}(?:\.h\b|manual|cross-check)|\bsdk\s+reference')
    for _, _, oid, path in blobs:
        end = output.index(b'\n', cursor)
        size = int(output[cursor:end].split()[-1])
        data = output[end + 1:end + 1 + size]
        cursor = end + size + 2
        total += size
        if data.startswith((b'\x7fELF', b'MZ', b'O153', b'\x4f\x15\x3d\x1d')):
            binaries.append({'path': path, 'bytes': size, 'sha256': hashlib.sha256(data).hexdigest()})
        try:
            text = data.decode('utf-8')
        except UnicodeDecodeError:
            continue
        for number, line in enumerate(text.splitlines(), 1):
            if reference.search(line):
                references.append({'path': path, 'line': number, 'text': line.strip()})
        if path in ('tests/shaderCfgTests.cpp', 'tests/ShaderRecompilerComputeTests.cpp'):
            for match in re.finditer(r'\{[^{}]{50,}\}', text):
                words = re.findall(r'0x[0-9a-fA-F]{8}', match[0])
                if len(words) >= 24:
                    arrays.append({'path': path, 'line': text.count('\n', 0, match.start()) + 1,
                                   'hex_words': len(words)})
    print(json.dumps({'revision': revision, 'entries': len(records), 'blob_bytes': total,
                      'executable_magic_candidates': binaries, 'reference_candidates': references,
                      'large_shader_array_candidates': arrays,
                      'submodules': [{'path': r[3], 'commit': r[2]} for r in records if r[1] == 'commit'],
                      'scope': 'Committed tree only. No history or submodule contents; no distributability verdict.'}, indent=2))


if __name__ == '__main__':
    main()
