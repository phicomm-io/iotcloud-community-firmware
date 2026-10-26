#!/usr/bin/env python3
"""Copy the published catalog and hash-verified firmware to static hosting."""
import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
from urllib.error import HTTPError
from urllib.request import Request, urlopen
try:
    from products import MODEL_VARIANTS
    from release_version import parse_tag, parse_version
except ModuleNotFoundError:
    import sys
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    from products import MODEL_VARIANTS
    from release_version import parse_tag, parse_version


def write_latest(catalog, output):
    """Small device manifests: follow stable, and expose signed images only."""
    stable = catalog.get('stable')
    release = next((r for r in catalog['releases'] if r['tag'] == stable), None)
    if stable and release is None:
        raise ValueError('Stable release is absent from catalog')
    if release and (parse_tag(stable).prerelease or parse_version(release['version']).prerelease or release.get('prerelease') or release.get('channel', 'stable') != 'stable'):
        raise ValueError('Release candidates cannot be the stable latest release')
    if release and parse_tag(stable).version != release['version']:
        raise ValueError('Stable latest release version differs from its tag')
    # Legacy clients receive at most the immutable 1.0.0 bridge release.
    version = parse_version(release['version']).core if release else (0, 0, 0)
    bridge = release
    if version > (1, 0, 0):
        bridge = next((r for r in catalog['releases'] if r['tag'] == 'v1.0.0'), None)
        if bridge is None: raise ValueError('v1.0.0 bridge release is absent from catalog')
        if bridge.get('version') != '1.0.0' or bridge.get('prerelease') or bridge.get('channel', 'stable') != 'stable':
            raise ValueError('v1.0.0 bridge must be a stable release')
    for channel, selected in [('legacy', bridge), ('v1', release if version >= (1, 0, 0) else None)]:
        for model, supported in MODEL_VARIANTS.items():
            item = selected.get('models', {}).get(model, {}) if selected else {}
            variants = dict(item.get('variants', {}))
            # Remove stale manifests too when rolling back to an older catalog.
            for variant in supported[1:]: variants.setdefault(variant, {})
            for variant, entry in [('full', item), *variants.items()]:
                folder = output / 'ota' / model if channel == 'legacy' else output / 'ota' / 'v1' / model / variant
                if channel == 'legacy' and variant != 'full': folder /= variant
                target = folder / 'latest.json'
                if channel == 'legacy' and variant == 'hub':
                    target.unlink(missing_ok=True)
                    continue
                signed = entry.get('downloads', {}).get('signed')
                if not signed:
                    target.unlink(missing_ok=True)
                    continue
                document = dict(schema=1, tag=selected['tag'], version=selected['version'],
                                commit=selected['commit'], model=model, firmware_variant=variant,
                                **signed, release_log=selected['notes'])
                data = (json.dumps(document, ensure_ascii=False, separators=(',', ':')) + '\n').encode()
                if len(data) > 4096: raise ValueError('Device manifest exceeds 4096 bytes')
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(data)

def mirror(source, output, include_candidates=False, candidate_fetch=None):
    output = Path(output)
    def validate_path(path):
        parts = PurePosixPath(path)
        if not path.startswith('/releases/') or '..' in parts.parts:
            raise ValueError(f'Invalid release path: {path}')
    def fetch(path):
        validate_path(path)
        with urlopen(Request(source.rstrip('/') + path, headers={'User-Agent': 'curl/8.5.0'}), timeout=60) as response:
            return response.read()
    catalog_bytes = fetch('/releases/index.json')
    catalog = json.loads(catalog_bytes)
    candidate_bytes = None
    candidates = []
    if include_candidates:
        fetch_candidate = candidate_fetch or fetch
        try:
            candidate_bytes = fetch_candidate('/releases/candidates.json')
        except HTTPError as error:
            if error.code != 404:
                raise
        if candidate_bytes is not None:
            candidate_catalog = json.loads(candidate_bytes)
            if (candidate_catalog.get('schema') != 1 or candidate_catalog.get('channel') != 'candidate' or
                    'stable' in candidate_catalog or not isinstance(candidate_catalog.get('releases'), list)):
                raise ValueError('Invalid candidate archive catalog')
            candidates = candidate_catalog['releases']
            if any(not parse_tag(r['tag']).prerelease or parse_tag(r['tag']).core[0] == 0 or parse_tag(r['tag']).version != r['version'] or r.get('channel') != 'candidate' or r.get('prerelease') is not True
                   for r in candidates):
                raise ValueError('Candidate archive contains a non-candidate release')
    for release in [*catalog['releases'], *candidates]:
        fetch_release = fetch_candidate if release in candidates else fetch
        assets = {}
        for model in release['models'].values():
            for entry in [model, *model.get('variants', {}).values()]:
                for value in entry['downloads'].values():
                    assets[value['path']] = value
        for path, value in assets.items():
            validate_path(path)
            if release in candidates and not path.startswith(f"/releases/{release['tag']}/"):
                raise ValueError('Candidate asset escapes its release directory')
            target = output / path.lstrip('/')
            data = target.read_bytes() if target.exists() else b''
            if hashlib.sha256(data).hexdigest() != value['sha256']:
                data = fetch_release(path)
            if len(data) != value['size'] or hashlib.sha256(data).hexdigest() != value['sha256']:
                raise ValueError(f'Firmware checksum mismatch: {path}')
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(data)
        for path in (release['notes'], f"/releases/{release['tag']}/SHA256SUMS"):
            validate_path(path)
            if release in candidates and not path.startswith(f"/releases/{release['tag']}/"):
                raise ValueError('Candidate note escapes its release directory')
            target = output / path.lstrip('/')
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(fetch_release(path))
    target = output / 'releases/index.json'
    target.parent.mkdir(parents=True, exist_ok=True)
    write_latest(catalog, output)
    target.write_bytes(catalog_bytes)
    if include_candidates:
        candidate_target = output / 'releases/candidates.json'
        if candidate_bytes is not None:
            candidate_target.write_bytes(candidate_bytes)
        else:
            candidate_target.unlink(missing_ok=True)
    print(f"Archived {len(catalog['releases'])} stable/archive and {len(candidates)} candidate releases with verified firmware hashes")


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output')
    parser.add_argument('--source', default='https://phicomm.fjrcn.com')
    parser.add_argument('--existing', action='store_true', help='Regenerate device manifests from the retained local release catalog')
    parser.add_argument('--include-candidates', action='store_true', help='Also retain the separate candidate catalog and hash-verified assets; stable/latest selection is unchanged')
    parser.add_argument('--candidates-r2', action='store_true', help='Read candidates through the authenticated R2 API while the stable Worker has no candidate routes')
    args = parser.parse_args()
    if args.candidates_r2 and not args.include_candidates:
        parser.error('--candidates-r2 requires --include-candidates')
    if args.existing:
        if args.include_candidates:
            parser.error('--include-candidates requires fetching an archive')
        output = Path(args.output)
        write_latest(json.loads((output / 'releases/index.json').read_text()), output)
    else:
        candidate_fetch = None
        if args.candidates_r2:
            from publish import get
            def candidate_fetch(path):
                body = get(path.lstrip('/'))
                if body is None:
                    raise HTTPError(path, 404, 'R2 candidate object absent', None, None)
                return body
        mirror(args.source, args.output, args.include_candidates, candidate_fetch)
