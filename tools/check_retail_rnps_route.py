"""Read-only RNPS route compatibility check; never stages or authenticates.

An explicitly supplied URL cache can have non-retail or unknown provenance.
Compatibility is not provenance: do not label its use strict retail.
"""
import argparse
import hashlib
import json
from pathlib import Path


def inspect(cache, apps):
    raw = cache.read_bytes()
    root = json.loads(raw)
    payload = root.get('urlconfig', root.get('response'))
    if not isinstance(payload, dict):
        raise ValueError('cache has no object payload')
    schemes = payload.get('scheme', {}).get('default', {})
    apps_config = payload.get('app', {})
    if not isinstance(schemes, dict) or not isinstance(apps_config, dict):
        raise ValueError('invalid scheme or app map')
    route = schemes.get('pssettings:play', schemes.get('pssettings'))
    if not isinstance(route, str) or not route.startswith('app:'):
        raise ValueError('no pssettings route')
    name = route[4:]
    application = apps_config.get(name, {}).get('default', {})
    rnps = application.get('rnps', {}) if isinstance(application, dict) else {}
    title, component = rnps.get('titleId'), rnps.get('component')
    if not isinstance(title, str) or not title.startswith('NPXS') or not title.isalnum():
        raise ValueError('invalid route title')
    manifest_path = apps / title / 'manifest.json'
    manifest_bytes = manifest_path.read_bytes()
    manifest = json.loads(manifest_bytes)
    if manifest.get('titleId') != title or manifest.get('applicationName') != name or component != 'Main':
        raise ValueError('route and retail manifest disagree')
    bundle = apps / title / 'application.ps.bundle'
    with bundle.open('rb') as stream:
        header = stream.read(8)
    return {'cache_sha256': hashlib.sha256(raw).hexdigest(),
            'settings_app': name, 'title_id': title, 'component': component,
            'manifest_sha256': hashlib.sha256(manifest_bytes).hexdigest(),
            'bundle_header': header.hex(), 'bundle_is_rnps_container': header == b'RNPSHEDR',
            'initialsetup_route_present': 'psinitialsetup:launched' in schemes or 'psinitialsetup' in schemes,
            'provenance': 'caller-supplied; not established by this check'}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cache', type=Path, required=True)
    parser.add_argument('--retail-apps', type=Path, required=True)
    args = parser.parse_args()
    print(json.dumps(inspect(args.cache, args.retail_apps), indent=2))


if __name__ == '__main__':
    main()
