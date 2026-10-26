#!/usr/bin/env python3
"""Review, compile and verify static user release notes; never build firmware."""
import argparse
import copy
import hashlib
import html
import json
import re
import subprocess
from pathlib import Path
from release_version import parse_tag, parse_version, version_key

ROOT = Path(__file__).resolve().parents[2]
DATA = ROOT / 'docs/release-notes'
LOCALES = ('zh-CN', 'en')
TYPES = ('feature', 'improvement', 'bugfix', 'security', 'breaking')
LABELS = {
    'zh-CN': dict(zip(TYPES, ('新功能', '功能改进', '问题修复', '安全更新', '重要变化'))),
    'en': dict(zip(TYPES, ('New features', 'Improvements', 'Bug fixes', 'Security updates', 'Important changes'))),
}


def digest(data):
    return hashlib.sha256(data).hexdigest()


def encoded(value):
    return (json.dumps(value, ensure_ascii=False, sort_keys=True, separators=(',', ':')) + '\n').encode()


def read(path):
    return json.loads(Path(path).read_bytes())


def version(value):
    return version_key(value)


def blocks(text):
    """Preserve exact block text; indices and hashes make reviews reproducible."""
    result, current = [], []
    for line in text.splitlines():
        if not line.strip() or (re.match(r'^[-*] ', line) and current):
            if current:
                result.append('\n'.join(current))
                current = []
        if line.strip():
            current.append(line)
    if current:
        result.append('\n'.join(current))
    return result


def migrate(data=DATA):
    catalog = read(data / 'release-order.json')
    reviews_path = data / 'reviews.json'
    reviews = read(reviews_path)['blocks'] if reviews_path.exists() else {}
    records = []
    for release in sorted(catalog['releases'], key=lambda r: version(r['version'])):
        tag = 'v' + release['version']
        raw = (data / 'sources' / (tag + '.md')).read_bytes()
        for index, text in enumerate(blocks(raw.decode()), 1):
            key = f"{tag}:b{index:03}"
            proof = dict(block_sha256=digest(text.encode()), release_sha256=digest(raw))
            review = reviews.get(key, {})
            valid = all(review.get(k) == v for k, v in proof.items())
            records.append(dict(block_id=key, tag=tag, text=text, **proof,
                                review=copy.deepcopy(review) if valid else {
                                    'status': 'pending', 'reason': 'Review missing or source changed',
                                    'invalidated': bool(review)}))
    return dict(schema=1, records=records)


def write_records(data=DATA):
    document = migrate(data)
    path = data / 'records.json'
    path.write_bytes(encoded(document))
    return document


def targets(release):
    for model, base in release['models'].items():
        yield model, 'full', base
        for variant, entry in base.get('variants', {}).items():
            yield model, variant, entry


def valid_path(archive, value):
    path = value.get('path', '')
    if not path.startswith('/releases/') or '..' in Path(path).parts:
        raise ValueError('Invalid archive path')
    target = (archive / path.lstrip('/')).resolve()
    if not target.is_relative_to((archive / 'releases').resolve()):
        raise ValueError('Archive path escapes releases/')
    return target


def verify_archive(catalog, identities, archive, data=DATA):
    """Formal builds read real immutable files and the historical source locks."""
    from verify import verify
    for release in catalog['releases']:
        tag = release['tag']
        notes = valid_path(archive, dict(path=release['notes'])).read_bytes()
        if notes != (data / 'sources' / (tag + '.md')).read_bytes():
            raise ValueError('Published original notes changed: ' + tag)
        checked = set()
        for model, variant, entry in targets(release):
            identity = identities['releases'][tag]['targets'][model + ':' + variant]
            source = entry.get('source_commit', release['models'][model].get('source_commit', release['commit']))
            if source != identity['source_commit']:
                raise ValueError('Archived source commit mismatch: ' + tag)
            frontend = identity['frontend']
            lock = subprocess.run(['git', 'show', source + ':frontend.lock.json'], cwd=ROOT, capture_output=True)
            if frontend['kind'] == 'lock':
                if lock.returncode or digest(lock.stdout) != frontend['sha256'] or json.loads(lock.stdout) != frontend['lock']:
                    raise ValueError('Historical frontend lock mismatch: ' + tag)
            elif lock.returncode == 0 or frontend != dict(kind='in-tree', source_commit=source):
                raise ValueError('Invalid historical in-tree frontend identity: ' + tag)
            for kind, asset in entry['downloads'].items():
                if asset['path'] in checked:
                    continue
                checked.add(asset['path'])
                body = valid_path(archive, asset).read_bytes()
                if len(body) != asset['size'] or digest(body) != asset['sha256']:
                    raise ValueError('Archived firmware hash mismatch: ' + asset['path'])
                if kind == 'signed':
                    verify(body, model, tag, source)


def compile_manifest(data=DATA, archive=None, formal=False, include_candidates=False):
    catalog_bytes = ((archive / 'releases/index.json') if archive else data / 'catalog.snapshot.json').read_bytes()
    catalog = json.loads(catalog_bytes)
    if include_candidates:
        if archive is None:
            raise ValueError('Candidate notes require an explicit local archive')
        candidates = read(archive / 'releases/candidates.json')
        if (candidates.get('schema') != 1 or candidates.get('channel') != 'candidate' or
                'stable' in candidates or not isinstance(candidates.get('releases'), list)):
            raise ValueError('Invalid candidate note catalog')
        for release in candidates['releases']:
            if not parse_tag(release['tag']).prerelease:
                raise ValueError('Candidate note catalog contains a stable release')
            existing = next((r for r in catalog['releases'] if r['tag'] == release['tag']), None)
            if existing is not None and existing != release:
                raise ValueError('Candidate note catalog conflicts with archived metadata')
            if existing is None:
                catalog['releases'].append(release)
        catalog['releases'].sort(key=lambda r: version(r['tag']), reverse=True)
        catalog_bytes = encoded(catalog)
    if parse_tag(catalog['stable']).prerelease:
        raise ValueError('Release candidates cannot replace the stable release-note selection')
    identities = read(data / 'archive-identities.json')
    declarations = {r['version']: r for r in read(data / 'release-order.json')['releases']}
    if len(declarations) != len(read(data / 'release-order.json')['releases']):
        raise ValueError('Duplicate release-chain declaration')
    known = read(data / 'catalog.snapshot.json')
    actual = {r['tag']: r for r in catalog['releases']}
    if len(actual) != len(catalog['releases']):
        raise ValueError('Duplicate archive release')
    if any(actual.get(r['tag']) != r for r in known['releases']):
        raise ValueError('Previously archived metadata changed or is missing')
    if digest((data / 'catalog.snapshot.json').read_bytes()) != identities['catalog_sha256']:
        raise ValueError('Historical archive snapshot hash mismatch')
    for release in catalog['releases']:
        identity = parse_tag(release['tag'])
        if (release['version'] != identity.version or
                release.get('channel', 'stable') != identity.channel or
                (identity.prerelease and release.get('prerelease') is not True) or
                (not identity.prerelease and release.get('prerelease'))):
            raise ValueError('Release version or channel differs from its tag')
        if release['version'] not in declarations:
            raise ValueError('Published release has no explicit reviewed chain declaration')
        declaration = declarations[release['version']]
        if declaration['branch'] != 'main' or declaration['channel'] != identity.channel:
            raise ValueError('Release declaration must match its explicit main/channel identity')
        if release['tag'] not in identities['releases']:
            if not archive or not formal:
                raise ValueError('New release identities require a formal build and actual archive')
            target_proofs = {}
            for model, variant, entry in targets(release):
                if 'signed' not in entry['downloads']:
                    raise ValueError('New releases require signed firmware for every target')
                source = entry.get('source_commit', release['models'][model].get('source_commit', release['commit']))
                lock = subprocess.run(['git', 'show', source + ':frontend.lock.json'], cwd=ROOT, capture_output=True)
                if lock.returncode:
                    raise ValueError('New releases require a verifiable frontend lock')
                target_proofs[model + ':' + variant] = dict(source_commit=source,
                    frontend=dict(kind='lock', sha256=digest(lock.stdout), lock=json.loads(lock.stdout)))
            identities['releases'][release['tag']] = dict(tag_commit=release['commit'], targets=target_proofs,
                notes_sha256=digest((data / 'sources' / (release['tag'] + '.md')).read_bytes()))
    if formal and not archive:
        raise ValueError('Formal builds require the actual archive')
    frozen = (ROOT / 'docs/release/Release-Log.md').read_bytes()
    if digest(frozen) != read(ROOT / 'docs/release/wiki-snapshot.json')['sha256']:
        raise ValueError('Frozen Wiki hash mismatch')
    if formal:
        verify_archive(catalog, identities, archive, data)
    records = migrate(data)['records']
    by_id = {r['block_id']: r for r in records}
    edits = read(data / 'changes.json')
    approved, pending, sources = [], [], {}
    all_changes = edits['changes']
    for row in all_changes:
        if 'v' + row['version'] not in actual:
            continue
        ref = row['change_id'] + '@' + row['version']
        proofs = [by_id.get(key) for key in row.get('source_refs', [])]
        valid = bool(proofs) and all(p and p['review'].get('status') == 'approved' and
                                   ref in p['review'].get('change_refs', []) for p in proofs)
        if row.get('review_status') != 'approved' or not valid:
            pending.append(row)
            continue
        if row['type'] not in TYPES or row['component'] not in ('firmware', 'bundled_web', 'service'):
            raise ValueError('Invalid change type/component: ' + ref)
        if row['lifecycle']['state'] not in ('active', 'withdrawn', 'superseded'):
            raise ValueError('Invalid lifecycle: ' + ref)
        scope = row['scope']
        if not all(isinstance(scope.get(k), list) and scope[k] and len(scope[k]) == len(set(scope[k]))
                   for k in ('models', 'variants', 'branches', 'channels', 'hardware')):
            raise ValueError('Invalid scope: ' + ref)
        if scope['branches'] != ['main'] or scope['channels'] != [parse_version(row['version']).channel]:
            raise ValueError('Change scope must match the explicit release channel')
        public = {k: copy.deepcopy(row[k]) for k in ('change_id', 'version', 'type', 'component', 'scope', 'review_status',
                  'lifecycle', 'title', 'description', 'ota_summary', 'summary_complete', 'critical')}
        # No fallback: incomplete locales remain visibly incomplete coverage.
        approved.append(public)
    releases = []
    for release in sorted(catalog['releases'], key=lambda r: version(r['tag'])):
        tag, ver = release['tag'], release['version']
        channel = parse_tag(tag).channel
        original = (data / 'sources' / (tag + '.md')).read_bytes()
        proof = identities['releases'][tag]
        if proof['tag_commit'] != release['commit'] or proof['notes_sha256'] != digest(original):
            raise ValueError('Original archive provenance changed: ' + tag)
        release_records = [r for r in records if r['tag'] == tag]
        release_pending = any(r['review']['status'] == 'pending' for r in release_records)
        coverage, target_rows = [], []
        for model, variant, entry in targets(release):
            info = proof['targets'][model + ':' + variant]
            source = info['source_commit']
            source_row = dict(commit=source, frontend=info['frontend'])
            if source in sources and sources[source] != source_row:
                raise ValueError('Conflicting frontend source proof')
            sources[source] = source_row
            kind = 'signed' if 'signed' in entry['downloads'] else 'raw'
            asset = entry['downloads'][kind]
            target_rows.append(dict(model=model, variant=variant, hardware='*', kind=kind,
                                    tag_commit=release['commit'], source_commit=source,
                                    frontend=info['frontend'], **asset))
            applicable = [c for c in all_changes if c['component'] != 'service' and c['version'] == ver and model in c['scope']['models'] and variant in c['scope']['variants']]
            incomplete = any(c in pending for c in applicable)
            languages = [lang for lang in LOCALES if all(c.get(field, {}).get(lang, '').strip()
                         for c in applicable for field in ('title', 'description'))]
            scope = dict(models=[model], variants=[variant], branches=['main'], channels=[channel], hardware=['*'])
            coverage.append(dict(scope=scope, status='pending' if release_pending or incomplete else 'complete', locales=languages))
        declaration = declarations[ver]
        if declaration['previous'] is not None and 'v' + declaration['previous'] not in actual:
            raise ValueError('Explicit release chain is broken')
        if declaration['previous'] is not None and version(declaration['previous']) >= version(ver):
            raise ValueError('Explicit release chain is out of order')
        if not parse_version(ver).prerelease and declaration['previous'] is not None and parse_version(declaration['previous']).prerelease:
            raise ValueError('Stable release chains cannot depend on a candidate release')
        releases.append(dict(version=ver, tag=tag, previous=declaration['previous'], branch=declaration['branch'], channel=declaration['channel'],
                             baseline_confirmed=not release_pending, coverage=coverage, targets=target_rows,
                             history_status='insufficient' if tag == 'v0.9.0' else 'pending' if release_pending else
                             'reviewed' if any(c['version'] == ver for c in approved) else 'no_user_changes'))
    merges = []
    for row in edits.get('merges', []):
        keys = row.get('source_refs', [])
        if not keys or any(k not in by_id or by_id[k]['review']['status'] != 'approved' for k in keys):
            raise ValueError('Merged summary has missing or invalidated reviews')
        merges.append({k: copy.deepcopy(v) for k, v in row.items() if k != 'source_refs'})
    supported_scopes = [dict(branch='main', channel='stable')]
    if any(parse_version(r['version']).prerelease for r in releases):
        supported_scopes.append(dict(branch='main', channel='candidate'))
    manifest = dict(schema=1, supported_scopes=supported_scopes,
                    stable=catalog['stable'].lstrip('v'), archive_sha256=digest(catalog_bytes),
                    releases=releases, changes=approved, merges=merges, sources=sources)
    return manifest, catalog_bytes


def immutable(path, body):
    if path.exists() and path.read_bytes() != body:
        raise ValueError('Immutable release-note resource changed: ' + str(path))
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(body)


def write_public(output, data=DATA, archive=None, formal=False, include_candidates=False):
    manifest, catalog_bytes = compile_manifest(data, archive, formal, include_candidates)
    body = encoded(manifest)
    name = 'manifest.' + digest(body) + '.json'
    directory = output / 'releases/notes'
    if include_candidates:
        directory /= 'candidate'
    immutable(directory / name, body)
    if not include_candidates:
        for source in (ROOT / 'site/release-notes').glob('*.js'):
            (directory / source.name).write_bytes(source.read_bytes())
    prefix = '/releases/notes/candidate/' if include_candidates else '/releases/notes/'
    document = dict(schema=1, manifest=dict(path=prefix + name, size=len(body), sha256=digest(body)),
                    catalog=dict(path='/releases/index.json', size=len(catalog_bytes), sha256=digest(catalog_bytes)))
    if include_candidates:
        catalog_name = 'catalog.' + digest(catalog_bytes) + '.json'
        immutable(directory / catalog_name, catalog_bytes)
        document.update(channel='candidate', source=dict(scope='candidate', kind='relative-archive'))
        document['catalog']['path'] = prefix + catalog_name
    # This is the only mutable pointer; write it after every immutable resource.
    index = directory / 'index.json'
    temporary = directory / 'index.json.tmp'
    temporary.write_bytes(encoded(document))
    temporary.replace(index)
    return manifest


def verify_output(output, data=DATA, formal=False, include_candidates=False):
    """Last build step: pointers, archived identity and rendered pages agree."""
    pointer = read(output / ('releases/notes/candidate/index.json' if include_candidates else 'releases/notes/index.json'))
    if include_candidates and (pointer.get('channel') != 'candidate' or pointer.get('source') != dict(scope='candidate', kind='relative-archive') or any(
            not re.fullmatch(r'/releases/notes/candidate/' + key + r'\.[a-f0-9]{64}\.json', pointer[key]['path']) for key in ('manifest', 'catalog'))):
        raise ValueError('Invalid candidate note snapshot identity')
    for key in ('manifest', 'catalog'):
        entry = pointer[key]
        body = valid_path(output, entry).read_bytes()
        if len(body) != entry['size'] or digest(body) != entry['sha256']:
            raise ValueError('Final public resource hash mismatch: ' + key)
    manifest = read(valid_path(output, pointer['manifest']))
    if manifest['archive_sha256'] != pointer['catalog']['sha256']:
        raise ValueError('Final notes/catalog snapshot mismatch')
    rebuilt, _ = compile_manifest(data, output if formal or include_candidates else None, formal, include_candidates)
    if manifest != rebuilt:
        raise ValueError('Compiled notes changed during page generation')
    # Candidate notes are optional browser data. Stable no-JavaScript history
    # remains the default document and keeps its existing rendering check.
    if include_candidates:
        return manifest
    for locale, path in [('zh-CN', 'guides/release-log.html'), ('en', 'guides/en/release-log.html')]:
        page = (output / path).read_text()
        if history_html(manifest, locale) not in page:
            raise ValueError('Rendered release history differs from the manifest')
    return manifest


def pending_report(data=DATA):
    return dict(schema=1, pending=[dict(block_id=r['block_id'], reason=r['review'].get('reason', ''),
        invalidated=r['review'].get('invalidated', False)) for r in migrate(data)['records']
        if r['review']['status'] == 'pending'])


def history_html(manifest, locale):
    esc = lambda s: html.escape(str(s), quote=True)
    out = []
    for release in reversed(manifest['releases']):
        out.append(f'<section class="notes-history-version" id="{release["tag"]}"><h2>{release["tag"]}</h2>')
        status = release['history_status']
        messages = {
            'zh-CN': {'insufficient': '仅有提交链接，资料不足，暂不能生成升级差异。', 'pending': '部分记录仍在整理；以下仅列已审核内容，涉及本版的完整升级差异暂停提供。', 'no_user_changes': '本版没有新增用户可见变化，与上一版使用相同实现。'},
            'en': {'insufficient': 'Only a commit link is available. There is insufficient information for an upgrade comparison.', 'pending': 'Some records are under review. Only approved notes are shown; complete comparisons involving this release are unavailable.', 'no_user_changes': 'No new user-visible changes. This release uses the same implementation as the previous release.'},
        }
        if status in messages[locale]:
            out.append('<p class="notes-coverage">' + messages[locale][status] + '</p>')
        for component, component_title in [('firmware', '固件' if locale == 'zh-CN' else 'Firmware'),
                                            ('bundled_web', '随固件网页' if locale == 'zh-CN' else 'Bundled device pages'),
                                            ('service', '独立平台服务（不计入固件 OTA）' if locale == 'zh-CN' else 'Independent services (excluded from firmware OTA)')]:
            rows = [r for r in manifest['changes'] if r['version'] == release['version'] and r['component'] == component]
            if not rows:
                continue
            out.append('<h3>' + component_title + '</h3>')
            for kind in TYPES:
                items = [r for r in rows if r['type'] == kind]
                if not items:
                    continue
                out.append('<h4>' + LABELS[locale][kind] + '</h4><ul class="notes-reviewed">')
                for row in items:
                    if not all(row.get(k, {}).get(locale) for k in ('title', 'description')):
                        out.append('<li>' + ('翻译待审核' if locale == 'zh-CN' else 'Translation pending review') + '</li>')
                        continue
                    scope = ', '.join(m.upper() for m in row['scope']['models']) + ' · ' + ', '.join(row['scope']['variants'])
                    out.append(f'<li><b>{esc(row["title"][locale])}</b><p>{esc(row["description"][locale])}</p><small>{esc(scope)}</small></li>')
                out.append('</ul>')
        out.append('</section>')
    return ''.join(out)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--data', type=Path, default=DATA)
    parser.add_argument('--migrate', action='store_true')
    parser.add_argument('--pending', action='store_true', help='Print the private review backlog')
    parser.add_argument('--verify-output', type=Path)
    parser.add_argument('--output', type=Path)
    parser.add_argument('--archive', type=Path)
    parser.add_argument('--formal', action='store_true')
    parser.add_argument('--include-candidates', action='store_true', help='Generate/verify a separate local candidate snapshot; never replace stable note or catalog pointers')
    args = parser.parse_args()
    if args.include_candidates and not (args.output or args.verify_output):
        parser.error('--include-candidates requires --output or --verify-output')
    if args.pending:
        print(encoded(pending_report(args.data)).decode(), end='')
    elif args.verify_output:
        verify_output(args.verify_output, args.data, args.formal, args.include_candidates)
        print('Final release-note consistency verified')
    elif args.migrate:
        result = write_records(args.data)
        print(f"Migrated {len(result['records'])} deterministic source blocks")
    elif args.output:
        manifest = write_public(args.output, args.data, args.archive, args.formal, args.include_candidates)
        print(f"Compiled {len(manifest['releases'])} releases and {len(manifest['changes'])} reviewed revisions")
    else:
        parser.error('choose --migrate or --output')


if __name__ == '__main__':
    main()
