"""Offline, metadata-only retail PSN prerequisite audit; never authenticates.

Accepts the original retail ShellCore and/or an explicitly supplied frozen guest
log. Does not search stores, read auth.dat, contact Sony, unwrap a secret, or
change firmware. The log reducer emits fixed marker names/counts/line numbers,
never source lines, user IDs, URLs, headers or bodies. Requires cryptography and
pyelftools, as does verify_psn_oauth_envelopes_1360.py.
"""
import argparse
import hashlib
import io
import json
from pathlib import Path
import re

from cryptography.hazmat.primitives.asymmetric import rsa
from elftools.elf.elffile import ELFFile

from verify_psn_oauth_envelopes_1360 import verify_envelope

CORE_SHA256 = 'f2e569cebe324129a8be6a40a429e6f6652021f261ded3c02a5469cc81f5a434'
NP_MANAGER_SHA256 = 'b39cd2012402c9ca6b95f2ae70704203bdb60a924c702cc0525fe206a5efb73d'
PUBLIC_KEY_RVA = 0x1f2c350
ENVELOPE_RVAS = (0x1f2b750, 0x1f2b930)
# Exact exported function extents, independently compared byte-for-byte with
# the earlier native transport audit. This is not a whole-image equivalence.
NP_EXPORT_WINDOWS = (
    ('private_create', 0xfc90, 0x71, '63f1556fae73a861c79707bbcaeee44fe9777a13fe2b98a0b613fa226ada0bbc'),
    ('ordinary_ticket', 0x125e0, 0x103, 'b0ad1aca35256ef38fc0f4139d498b42f42f89b5f422249e55ed6924fc336e28'),
    ('login_ticket', 0x135e0, 0xe8, '29fff8c5cbfcfaa737058ae345696d30fab11fd4291ed1b4106c2f77daa096a8'),
)


def read_virtual(image, loads, address, size):
    if address < 0 or size < 0:
        raise ValueError('Negative virtual extent')
    matches = []
    for segment in loads:
        delta = address - segment['p_vaddr']
        if 0 <= delta and delta + size <= segment['p_filesz']:
            start = segment['p_offset'] + delta
            if start >= 0 and start + size <= len(image):
                matches.append(image[start:start + size])
    if len(matches) != 1:
        raise ValueError('Virtual extent must have exactly one file-backed mapping')
    return matches[0]


def inspect_core(path):
    image = path.read_bytes()
    digest = hashlib.sha256(image).hexdigest()
    if digest != CORE_SHA256:
        raise ValueError('Wrong source: only the pinned original retail 13.60 Core is supported')
    elf = ELFFile(io.BytesIO(image))
    loads = [dict(s.header) for s in elf.iter_segments() if s['p_type'] == 'PT_LOAD']
    public = read_virtual(image, loads, PUBLIC_KEY_RVA, 0x300)
    key = rsa.RSAPublicNumbers(int.from_bytes(public[0x180:], 'big'),
                              int.from_bytes(public[:0x180], 'big')).public_key()
    envelopes = []
    for address in ENVELOPE_RVAS:
        envelope = read_virtual(image, loads, address, 0x1e0)
        envelopes.append({'rva': hex(address), **verify_envelope(envelope, key)})
    return {'source_sha256': digest, 'public_key_rva': hex(PUBLIC_KEY_RVA),
            'public_key_sha256': hashlib.sha256(public).hexdigest(),
            'envelopes': envelopes, 'plaintext_recovered': False,
            'psn_authentication_verified': False}


def inspect_np_manager(path):
    image = path.read_bytes()
    digest = hashlib.sha256(image).hexdigest()
    if digest != NP_MANAGER_SHA256:
        raise ValueError('Wrong source: only the pinned original retail 13.60 NP manager is supported')
    elf = ELFFile(io.BytesIO(image))
    loads = [dict(s.header) for s in elf.iter_segments() if s['p_type'] == 'PT_LOAD']
    exports = []
    for name, address, size, expected in NP_EXPORT_WINDOWS:
        actual = hashlib.sha256(read_virtual(image, loads, address, size)).hexdigest()
        if actual != expected:
            raise ValueError('Retail native contract window mismatch')
        exports.append({'name': name, 'rva': hex(address), 'size': hex(size), 'sha256': actual})
    return {'source_sha256': digest, 'reviewed_export_windows': exports,
            'limitation': 'Static function bytes only; no native execution or PSN authentication.'}


# These patterns detect observations, not success or absence of an operation.
# In particular an envelope open at boot can belong to telemetry, not OAuth.
MARKERS = {
    'envelope_open_missing': re.compile(r'^ps5-vsh: open ENOENT: /dev/envelope(?: |$)'),
    'pltauth_open_missing': re.compile(r'^ps5-vsh: open ENOENT: /dev/pltauth[01](?: |$)'),
    'pltauth_open_attempt': re.compile(r'^\s*path = /dev/pltauth[01]\s*$'),
    'np_owner_endpoint_unavailable': re.compile(
        r'^NP manager client: \w+ service=SceNpMgrIpc result=0x80550002 owner_endpoint_unavailable\s*$'),
    'local_profile_not_signed_up': re.compile(
        r'^NP local-account HLE: pid=\d+ user=0x[0-9a-fA-F]+ state=1 \(local profile, no online session\)\s*$'),
    'local_profile_signed_out': re.compile(
        r'^NP local-account HLE: pid=\d+ user=0x[0-9a-fA-F]+ state=2 \(local profile, no online session\)\s*$'),
    'psncore_startup_ok_only': re.compile(r'^\[SceShellCore\] PsnCore OK, \d+ sec\s*$'),
}


def inspect_frozen_log(path):
    digest = hashlib.sha256()
    observations = {name: {'count': 0, 'first_line': None, 'last_line': None} for name in MARKERS}
    total = 0
    with path.open('rb') as source:
        for total, raw in enumerate(source, 1):
            digest.update(raw)
            line = raw.decode('utf-8', errors='replace').rstrip('\r\n')
            for name, pattern in MARKERS.items():
                if pattern.search(line):
                    row = observations[name]
                    row['count'] += 1
                    if row['first_line'] is None:
                        row['first_line'] = total
                    row['last_line'] = total
    return {'source_sha256': digest.hexdigest(), 'lines': total, 'observations': observations,
            'raw_lines_emitted': False, 'psn_authentication_verified': False,
            'limitation': 'Marker absence is not proof of operation absence; boot envelope failures can be telemetry.'}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--core', type=Path, help='Original, unmodified retail ShellCore')
    parser.add_argument('--np-manager', type=Path, help='Original retail libSceNpManager.sprx')
    parser.add_argument('--frozen-guest-log', type=Path, help='Explicit immutable log only; not a live capture')
    parser.add_argument('--output', type=Path, help='New JSON file, exclusively created')
    args = parser.parse_args()
    if args.core is None and args.np_manager is None and args.frozen_guest_log is None:
        parser.error('Supply --core, --np-manager and/or --frozen-guest-log')
    report = {'mode': 'offline_prerequisite_audit_not_authentication'}
    if args.core is not None:
        report['retail_core'] = inspect_core(args.core)
    if args.np_manager is not None:
        report['retail_np_manager'] = inspect_np_manager(args.np_manager)
    if args.frozen_guest_log is not None:
        report['frozen_log'] = inspect_frozen_log(args.frozen_guest_log)
    text = json.dumps(report, indent=2) + '\n'
    if args.output is not None:
        with args.output.open('x', encoding='utf-8', newline='\n') as output:
            output.write(text)
    print(text, end='')


if __name__ == '__main__':
    main()
