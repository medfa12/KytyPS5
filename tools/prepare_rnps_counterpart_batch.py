"""Stage the requested 13.60 RNPS counterparts as an explicit mixed comparison.

No decryption or guest execution. Preflight the whole batch before creating a
new stage. Keep matching retail manifests/assets; copy exact plaintext bytes.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct

from prepare_retail_host_comparison import FILES as HOST_PINS, validate_pair

APP_TITLES = tuple('NPXS' + n for n in (
    '40009', '40002', '40003', '40007', '40008', '40011', '40021', '40081',
    '40147', '40260', '40013', '40051', '40103', '40182'))
HOST_TITLE = 'NPXS40141'
HOME_FOLLOWUP_TITLES = ('NPXS40048', 'NPXS40024', 'NPXS40062')
HOME_LIBRARY_TITLES = ('NPXS40071',)
HBC_MAGIC = bytes.fromhex('c61fbc03c103191f')
GENERATED = {'application.ps.hbc', 'application.ps.js', 'decompiler.stderr.log',
             'decompiler.stdout.log', 'application.ps.bundle.orig'}


def digest(data):
    return hashlib.sha256(data).hexdigest()


def inspect_hbc(data):
    if data[:8] == b'RNPSHEDR':
        raise ValueError('Still RNPSHEDR; no plaintext counterpart')
    if len(data) < 128 or data[:8] != HBC_MAGIC:
        raise ValueError('Missing Hermes bytecode header')
    version = struct.unpack_from('<I', data, 8)[0]
    size = struct.unpack_from('<I', data, 32)[0]
    if version != 89 or size != len(data):
        raise ValueError('Unsupported/truncated Hermes file')
    if hashlib.sha1(data[:-20]).digest() != data[-20:]:
        raise ValueError('Hermes footer digest mismatch')
    return dict(format='plaintext Hermes bytecode', version=version,
                header_hex=data[:36].hex(), declared_file_bytes=size,
                footer_sha1_verified=True)


def container_info(data):
    if len(data) < 48 or data[:8] != b'RNPSHEDR':
        raise ValueError('Expected original RNPSHEDR container')
    offset, stored, plain = struct.unpack_from('<III', data, 28)
    if offset > len(data) or stored > len(data) - offset or plain <= 0:
        raise ValueError('Invalid RNPS container lengths')
    return dict(sha256=digest(data), bytes=len(data), payload_offset=offset,
                stored_bytes=stored, declared_plaintext_bytes=plain)


def tree(directory):
    if not directory.is_dir():
        raise ValueError(f'Missing title: {directory}')
    return {p.relative_to(directory).as_posix(): p for p in sorted(directory.rglob('*')) if p.is_file()}


def sibling(name):
    # Settings also ships appdb descriptors/icons; both trees must agree on
    # these nested resources just as they do on assets and the manifest.
    return name.startswith(('assets/', 'appdb/')) or name in ('manifest.json', 'license.txt', 'application.ps.bundle.LICENSE.txt')


def prepare(retail_apps, counterparts, host_comparison, apps_comparison, stage, titles=None):
    selected_titles = (*APP_TITLES, HOST_TITLE) if titles is None else tuple(titles)
    allowed = {*APP_TITLES, HOST_TITLE, *HOME_FOLLOWUP_TITLES, *HOME_LIBRARY_TITLES}
    if not selected_titles or len(set(selected_titles)) != len(selected_titles) or any(t not in allowed for t in selected_titles):
        raise ValueError('Requested titles must be nonempty, unique and in the reviewed counterpart set')
    retail_apps, counterparts, stage = (p.resolve() for p in (retail_apps, counterparts, stage))
    protected = [retail_apps, counterparts]
    if host_comparison:
        host_comparison = host_comparison.resolve()
        protected.append(host_comparison)
    if HOST_TITLE in selected_titles and host_comparison is None:
        raise ValueError('Host title requires the prior audited host comparison')
    if apps_comparison:
        apps_comparison = apps_comparison.resolve()
        protected.append(apps_comparison)
    if stage.exists() or any(stage == p or stage in p.parents or p in stage.parents for p in protected):
        raise ValueError('Stage must be new and outside all source trees')
    sources = {}
    plans = []
    reports = {}

    def load(path):
        data = path.read_bytes()
        previous = sources.setdefault(str(path), digest(data))
        if previous != digest(data):
            raise ValueError(f'Source changed during audit: {path}')
        return data

    def plan(title, name, path, provenance, equivalent=None):
        data = load(path)
        row = dict(path=f'rnps/apps/{title}/{name}', source=str(path),
                   sha256=digest(data), bytes=len(data), provenance=provenance)
        if equivalent:
            if load(equivalent) != data:
                raise ValueError(f'Sibling mismatch: {title}/{name}')
            row['identical_counterpart'] = str(equivalent)
        plans.append(row)

    for title in selected_titles:
        original = tree(retail_apps / title)
        dev = tree(counterparts / title)
        manifest_bytes = load(original['manifest.json'])
        if manifest_bytes != load(dev['manifest.json']):
            raise ValueError(f'{title}: manifest differs; coherent counterpart selection needs review')
        manifest = json.loads(manifest_bytes)
        if manifest.get('titleId') != title:
            raise ValueError(f'{title}: manifest title mismatch')
        bundle_names = set(HOST_PINS) if title == HOST_TITLE else {'application.ps.bundle'}
        for inventory in (original, dev):
            unexpected = [n for n in inventory if not sibling(n) and n not in bundle_names and n not in GENERATED]
            if unexpected:
                raise ValueError(f'{title}: unreviewed sibling resources: {unexpected}')
        retail_siblings = {n for n in original if sibling(n)}
        if retail_siblings != {n for n in dev if sibling(n)}:
            raise ValueError(f'{title}: sibling asset inventory differs')
        for name in sorted(retail_siblings):
            plan(title, name, original[name], 'retail manifest/asset; exact counterpart match', dev[name])
        report = dict(manifest=manifest, manifest_sha256=digest(manifest_bytes),
                      manifest_and_siblings_identical=True, sibling_files=len(retail_siblings),
                      omitted_local_artifacts={label: sorted(set(inventory) & GENERATED)
                                               for label, inventory in (('retail', original), ('counterpart', dev))},
                      bundles={})
        reports[title] = report
        if title != HOST_TITLE:
            if manifest.get('jsBackend') != 'HERMES':
                raise ValueError(f'{title}: unsupported backend')
            name = 'application.ps.bundle'
            plain = load(dev[name])
            info = inspect_hbc(plain)
            wrapped = load(original[name])
            container = container_info(wrapped)
            if container['declared_plaintext_bytes'] != len(plain):
                raise ValueError(f'{title}: plaintext/container size mismatch')
            if load(dev['application.ps.bundle.orig']) != wrapped:
                raise ValueError(f'{title}: preserved counterpart container differs from retail')
            info['retail_container'] = container
            info['counterpart_orig_equals_retail'] = True
            if apps_comparison and title in ('NPXS40008', 'NPXS40011'):
                prior = apps_comparison / 'rnps/apps' / title / name
                if load(prior) != plain:
                    raise ValueError(f'{title}: differs from previously tested comparison')
                info['identical_prior_comparison'] = str(prior)
            report['bundles'][name] = info
            plan(title, name, dev[name], 'existing 13.60 counterpart plaintext; NOT authenticated retail decryption')
        else:
            # Requested counterparts remain encrypted. Reuse only the previously
            # audited, hash-pinned host/base pair with identical source containers.
            report['counterpart_status'] = 'RNPSHEDR; direct counterpart unusable, existing mixed plaintext reused'
            for name, (wrapped_pin, plain_pin) in HOST_PINS.items():
                wrapped = load(original[name])
                if load(dev[name]) != wrapped:
                    raise ValueError(f'{title}/{name}: counterpart container differs from audited retail')
                prior = host_comparison / 'rnps/apps' / title / name
                plain = load(prior)
                validate_pair(name, wrapped, plain, wrapped_pin, plain_pin)
                if plain[:8] == b'RNPSHEDR':
                    raise ValueError(f'{title}/{name}: comparison is still encrypted')
                info = dict(format='plaintext Hermes bytecode' if name.endswith('.hbc') else 'plaintext JavaScript',
                            header_hex=plain[:36].hex(), retail_container=container_info(wrapped),
                            exact_prior_comparison_sha256=plain_pin)
                if name.endswith('.hbc'):
                    info.update(inspect_hbc(plain), physical_file_bytes=len(plain))
                else:
                    text = plain.decode('utf-8')
                    if '\0' in text:
                        raise ValueError('Unexpected binary host JS')
                report['bundles'][name] = info
                plan(title, name, prior, 'prior audited mixed host plaintext; original devkit/retail containers identical')

    # All titles pass before claiming the fresh destination. No source mutation.
    stage.mkdir(parents=True, exist_ok=False)
    for row in plans:
        data = load(Path(row['source']))
        if digest(data) != row['sha256']:
            raise ValueError('Source changed after preflight')
        target = stage / row['path']
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)
        if target.read_bytes() != data:
            raise ValueError(f'Copy mismatch: {target}')
    for path, expected in sources.items():
        if digest(Path(path).read_bytes()) != expected:
            raise ValueError(f'Source changed during copying: {path}')
    mounts = [dict(guest=f'/system_ex/rnps/apps/{title}', host=str(stage / 'rnps/apps' / title), read_only=True)
              for title in reports]
    receipt = dict(classification='MIXED counterpart comparison; NOT authenticated retail decryption',
                   requested_titles=list(selected_titles),
                   titles=reports, files=plans, source_hashes=sources, mounts=mounts,
                   guest_runtime_verified=False,
                   limitations='Batch coverage is the requested titles only; headers/manifests/assets do not prove runtime compatibility.')
    (stage / 'comparison-receipt.json').write_text(json.dumps(receipt, indent=2) + '\n', encoding='utf-8')
    return receipt


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('retail-apps', 'counterparts', 'stage'):
        parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--host-comparison', type=Path)
    parser.add_argument('--apps-comparison', type=Path)
    parser.add_argument('--titles', nargs='+', choices=(*APP_TITLES, HOST_TITLE, *HOME_FOLLOWUP_TITLES, *HOME_LIBRARY_TITLES),
                        help='Explicit additive subset; default remains the original 15-title batch')
    args = parser.parse_args()
    receipt = prepare(args.retail_apps, args.counterparts, args.host_comparison, args.apps_comparison, args.stage, args.titles)
    print(json.dumps(dict(titles=len(receipt['titles']), files=len(receipt['files']),
                         receipt=str(args.stage / 'comparison-receipt.json'),
                         receipt_sha256=digest((args.stage / 'comparison-receipt.json').read_bytes())), indent=2))


if __name__ == '__main__':
    main()
