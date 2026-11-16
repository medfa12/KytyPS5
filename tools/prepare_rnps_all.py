"""Copy the COMPLETE retail RNPS tree into a fresh, audited mixed-source stage.

Use a counterpart only with matching retail resources, preserved original
container, and validated Hermes v89 bytecode. Preserve unresolved containers.
Never modify firmware, earlier stages, authenticated stores, or runtime state.
"""
import argparse
from collections import Counter
import json
from pathlib import Path

from prepare_rnps_counterpart_batch import (GENERATED, HOST_PINS, HOST_TITLE,
                                          container_info, digest, inspect_hbc)
from prepare_retail_host_comparison import validate_pair


def inventory(root):
    if not root.is_dir() or root.is_symlink():
        raise ValueError(f'Missing/linked source tree: {root}')
    result = {}
    for path in sorted(root.rglob('*')):
        if path.is_symlink():
            raise ValueError(f'Linked source entry: {path}')
        if path.is_file():
            data = path.read_bytes()
            result[path.relative_to(root).as_posix()] = dict(sha256=digest(data), bytes=len(data))
    return result


def generated(name):
    # These are known local app decompiler/original-copy artifacts. Do not
    # exclude genuine bgs *.js/*.hbc, nested assets, or shipped host *.bundle.hbc.
    parts = name.split('/')
    return len(parts) == 3 and parts[0] == 'apps' and parts[2] in GENERATED


def bundle(name):
    return (name.endswith(('.bundle', '.bundle.hbc', '/main.jsbundle')) or
            name.startswith('bgs/') and name.endswith(('.js', '.hbc')))


def prepare(retail, counterparts, host_comparison, stage):
    retail, counterparts, stage = (Path(p).resolve() for p in (retail, counterparts, stage))
    host_comparison = Path(host_comparison).resolve() if host_comparison else None
    roots = {'retail': retail, 'counterpart': counterparts}
    if host_comparison:
        roots['prior_host'] = host_comparison
    if stage.exists() or any(stage == p or stage in p.parents or p in stage.parents for p in roots.values()):
        raise ValueError('Stage must be new and outside every source tree')
    snapshots = {label: inventory(root) for label, root in roots.items()}

    def load(label, name):
        data = (roots[label] / name).read_bytes()
        expected = snapshots[label][name]
        if dict(sha256=digest(data), bytes=len(data)) != expected:
            raise ValueError(f'Source changed: {label}/{name}')
        return data

    originals = snapshots['retail']
    dev = snapshots['counterpart']
    omitted = {label: sorted(n for n in inv if generated(n)) for label, inv in snapshots.items()}
    selected = {n for n in originals if not generated(n)}
    titles = sorted({n.split('/')[1] for n in selected if n.startswith('apps/')})
    dev_titles = sorted({n.split('/')[1] for n in dev if n.startswith('apps/')})
    reports = {}
    for title in titles:
        prefix = f'apps/{title}/'
        names = {n for n in selected if n.startswith(prefix)}
        other_names = {n for n in dev if n.startswith(prefix) and not generated(n)}
        manifest_name = prefix + 'manifest.json'
        if manifest_name not in names:
            raise ValueError(f'Missing manifest: {title}')
        manifest = json.loads(load('retail', manifest_name))
        if manifest.get('titleId') != title:
            raise ValueError(f'Manifest title mismatch: {title}')
        siblings = {n for n in names if not bundle(n)}
        dev_siblings = {n for n in other_names if not bundle(n)}
        coherent = siblings == dev_siblings and all(originals[n] == dev[n] for n in siblings)
        reports[title] = dict(application_name=manifest.get('applicationName'),
                              js_backend=manifest.get('jsBackend'),
                              manifest_sha256=originals[manifest_name]['sha256'],
                              counterpart_resources_identical=coherent,
                              resource_files=len(siblings), bundles={})

    bgs_titles = sorted({n.split('/')[1] for n in selected if n.startswith('bgs/')})
    background_services = {}
    for title in bgs_titles:
        prefix = f'bgs/{title}/'
        names = {n for n in selected if n.startswith(prefix)}
        background_services[title] = dict(
            files=len(names), bundle_files=sum(bundle(n) for n in names),
            counterpart_inventory_and_bytes_identical=(names == {n for n in dev if n.startswith(prefix)} and
                                                       all(originals[n] == dev[n] for n in names)),
            bundles={})

    plans, unresolved = [], []
    for name in sorted(selected):
        data = load('retail', name)
        label, source_name = 'retail', name
        provenance = 'original retail bytes'
        detail = None
        title = name.split('/')[1] if name.startswith('apps/') else None
        bgs_title = name.split('/')[1] if name.startswith('bgs/') else None
        is_container = data[:8] == b'RNPSHEDR'
        if is_container:
            wrapped = container_info(data)  # Invalid retail containers fail preflight.
            reason = 'no verified plaintext counterpart'
            try:
                if bgs_title:
                    if name in dev and load('counterpart', name)[:8] == b'RNPSHEDR':
                        raise ValueError('background-service counterpart also RNPSHEDR; no verified plaintext')
                    raise ValueError('no audited background-service plaintext replacement')
                if not title or not reports[title]['counterpart_resources_identical']:
                    raise ValueError('missing counterpart title or differing resource inventory/bytes')
                if title == HOST_TITLE and name.rsplit('/', 1)[-1] in HOST_PINS:
                    short = name.rsplit('/', 1)[-1]
                    if host_comparison is None:
                        raise ValueError('prior audited host comparison not supplied')
                    if load('counterpart', name) != data:
                        raise ValueError('host counterpart container differs from retail')
                    source_name = 'rnps/' + name
                    plain = load('prior_host', source_name)
                    validate_pair(short, data, plain, *HOST_PINS[short])
                    if short.endswith('.hbc'):
                        detail = inspect_hbc(plain)
                    else:
                        if '\0' in plain.decode('utf-8'):
                            raise ValueError('binary host JavaScript')
                        detail = dict(format='prior hash-pinned JavaScript')
                    label = 'prior_host'
                    provenance = 'prior audited mixed host plaintext; NOT authenticated retail decryption'
                else:
                    if reports[title]['js_backend'] != 'HERMES':
                        raise ValueError('unsupported manifest backend for Hermes replacement')
                    plain = load('counterpart', name)
                    detail = inspect_hbc(plain)
                    if load('counterpart', name + '.orig') != data:
                        raise ValueError('preserved counterpart container differs from retail')
                    label, source_name = 'counterpart', name
                    provenance = 'authorized devkit plaintext; matching original container; NOT authenticated retail decryption'
                if wrapped['declared_plaintext_bytes'] != len(plain):
                    raise ValueError('plaintext/container length mismatch')
                detail.update(retail_container=wrapped, counterpart_original_equals_retail=True)
                data = plain
            except (ValueError, KeyError, OSError, UnicodeError) as error:
                label, source_name, detail = 'retail', name, None
                reason = str(error)
                unresolved.append(dict(path=name, reason=reason, sha256=originals[name]['sha256']))
                provenance = 'unresolved RNPSHEDR; original retail bytes preserved'
            if title:
                reports[title]['bundles'][name.rsplit('/', 1)[-1]] = dict(
                    status='verified plaintext structure, runtime unverified' if detail else 'encrypted/unresolved',
                    validation=detail, retail_container=wrapped)
            if bgs_title:
                background_services[bgs_title]['bundles'][name.split('/', 2)[2]] = dict(
                    status='encrypted/unresolved; original retail bytes preserved', retail_container=wrapped)
        elif bundle(name) and title:
            # No arbitrary non-container file is silently promoted to runnable.
            reports[title]['bundles'][name.rsplit('/', 1)[-1]] = dict(status='original retail bytes, runtime unverified')
        row = dict(path='rnps/' + name, source=str(roots[label] / source_name),
                   source_tree=label, source_relative_path=source_name,
                   sha256=digest(data), bytes=len(data), provenance=provenance)
        if name in dev and originals[name] == dev[name]:
            row['identical_counterpart_sha256'] = dev[name]['sha256']
        plans.append(row)

    # Preflight includes complete source inventory preservation, not only used
    # files; later additions/removals and changes to omitted artifacts fail too.
    def verify_sources():
        for label, root in roots.items():
            if inventory(root) != snapshots[label]:
                raise ValueError(f'Source inventory changed: {label}')

    verify_sources()
    stage.mkdir(parents=True, exist_ok=False)
    for row in plans:
        data = load(row['source_tree'], row['source_relative_path'])
        target = stage / row['path']
        target.parent.mkdir(parents=True, exist_ok=True)
        with target.open('xb') as stream:
            stream.write(data)
        if digest(target.read_bytes()) != row['sha256']:
            raise ValueError(f'Stage copy mismatch: {target}')
    verify_sources()
    counts = dict(app_titles=len(titles), bgs_titles=len(bgs_titles),
                  staged_files=len(plans), app_files=sum(n.startswith('apps/') for n in selected),
                  bgs_files=sum(n.startswith('bgs/') for n in selected),
                  bgs_script_files=sum(n.startswith('bgs/') and n.endswith(('.js', '.hbc')) for n in selected),
                  app_bundle_files=sum(n.startswith('apps/') and bundle(n) for n in selected),
                  bundle_files=sum(bundle(n) for n in selected),
                  resources=sum(not bundle(n) for n in selected),
                  substituted_plaintext_files=sum(r['source_tree'] != 'retail' for r in plans),
                  unresolved_containers=len(unresolved), source_files={k: len(v) for k, v in snapshots.items()})
    receipt = dict(schema_version=1,
                   classification='COMPLETE retail RNPS inventory with authorized MIXED plaintext counterparts; NOT authenticated retail decryption',
                   source_roots={k: str(v) for k, v in roots.items()},
                   source_inventories=snapshots, source_inventories_unchanged=True,
                   counts=counts, titles=reports, bgs_titles=bgs_titles,
                   background_services=background_services,
                   counterpart_only_titles_not_retail=sorted(set(dev_titles) - set(titles)),
                   omitted_local_artifacts=omitted, unresolved=unresolved, files=plans,
                   provenance_counts=dict(Counter(r['provenance'] for r in plans)),
                   mount=dict(guest='/system_ex/rnps', host=str(stage / 'rnps'), read_only=True),
                   guest_runtime_verified=False, all_bundles_runnable=False,
                   limitations='Complete RETAIL tree coverage, excluding recorded local artifacts. Counterpart-only titles are inventoried, not injected as retail titles. Structure/hash checks are not runtime or cryptographic authentication.')
    with (stage / 'comparison-receipt.json').open('x', encoding='utf-8') as stream:
        stream.write(json.dumps(receipt, indent=2) + '\n')
    return receipt


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('retail-rnps', 'counterpart-rnps', 'stage'):
        parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--host-comparison', type=Path)
    args = parser.parse_args()
    receipt = prepare(args.retail_rnps, args.counterpart_rnps, args.host_comparison, args.stage)
    print(json.dumps(dict(counts=receipt['counts'], mount=receipt['mount'],
                          unresolved_by_root=dict(Counter(r['path'].split('/')[0] for r in receipt['unresolved'])),
                          unresolved_app_titles=sorted({r['path'].split('/')[1] for r in receipt['unresolved'] if r['path'].startswith('apps/')}),
                          receipt=str(args.stage / 'comparison-receipt.json'),
                          receipt_sha256=digest((args.stage / 'comparison-receipt.json').read_bytes())), indent=2))


if __name__ == '__main__':
    main()
