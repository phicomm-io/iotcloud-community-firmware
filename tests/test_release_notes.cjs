'use strict';
const {test} = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs'), os = require('node:os'), path = require('node:path'), cp = require('node:child_process');
const Notes = require('../site/release-notes/core.js');
const copy = x => structuredClone(x);
const scope = (models = ['a1', 'm1', 'tc1'], variants = ['full', 'no-frontend-resources'], hardware = ['*']) =>
  ({models, variants, hardware, branches: ['main'], channels: ['stable']});
const words = value => ({'zh-CN': value, en: 'EN ' + value});
function revision(id, version, extra = {}) {
  return {change_id: id, version, type: 'feature', component: 'firmware', scope: scope(), review_status: 'approved',
    lifecycle: {state: 'active'}, summary_complete: true, title: words(id), description: words(id + ' description'), ota_summary: words(id + ' complete ' + version), ...extra};
}
function fixture(versions = ['0.9.0', '0.9.1', '0.9.2', '0.9.3']) {
  const manifest = {schema: 1, supported_scopes: [{branch: 'main', channel: 'stable'}], releases: [], changes: [], merges: [], sources: {}};
  const catalog = {schema: 1, releases: [], stable: 'v' + versions.at(-1)};
  for (const [index, version] of versions.entries()) {
    const commit = (index + 1).toString(16).padStart(40, '0'), frontend = {kind: 'in-tree', source_commit: commit};
    manifest.sources[commit] = {commit, frontend};
    const release = {version, previous: versions[index - 1] || null, branch: 'main', channel: 'stable', baseline_confirmed: true, targets: [], coverage: []};
    const archive = {version, tag: 'v' + version, commit, models: {}};
    for (const model of ['a1', 'm1', 'tc1']) for (const variant of ['full', 'no-frontend-resources']) {
      const asset = {path: `/releases/v${version}/${model}-${variant}.signed`, sha256: 'a'.repeat(64), size: 1024};
      const target = {model, variant, hardware: '*', kind: 'signed', tag_commit: commit, source_commit: commit, frontend: copy(frontend), ...asset};
      release.targets.push(target); release.coverage.push({scope: scope([model], [variant]), status: 'complete', locales: ['zh-CN', 'en']});
      const entry = {...asset, downloads: {signed: copy(asset)}};
      if (variant === 'full') archive.models[model] = {...entry, variants: {}};
      else archive.models[model].variants[variant] = entry;
    }
    manifest.releases.push(release); catalog.releases.push(archive);
  }
  return {manifest, catalog};
}
const query = {from: '0.9.0', to: '0.9.3', model: 'a1', variant: 'full'};
function run(f, q = {}) { return Notes.compare(f.manifest, {...query, ...q}, f.catalog); }
function failure(result, code) { assert.equal(result.ok, false); assert.equal(result.error.code, code); assert.equal('changes' in result, false); assert.equal('groups' in result, false); }

test('same version is empty after validating real device scope and identity', () => {
  const f = fixture(); assert.deepEqual(run(f, {from: 'v0.9.3'}).changes, []);
  failure(run(f, {from: '0.9.3', variant: 'fresh-air'}), 'TARGET_NOT_FOUND');
});
test('three revisions use the last complete state and do not re-add baseline features', () => {
  const f = fixture(); f.manifest.changes = [revision('fan', '0.9.1'), revision('fan', '0.9.2'), revision('fan', '0.9.3')];
  let result = run(f); assert.equal(result.ok, true); assert.equal(result.changes.length, 1); assert.match(result.changes[0].description, /0.9.3/);
  result = run(f, {from: '0.9.1'}); assert.equal(result.changes[0].type, 'improvement'); assert.equal(result.changes[0].impact, 'updated');
});
test('an independent bugfix survives aggregation of its related feature', () => {
  const f = fixture(); f.manifest.changes = [revision('fan', '0.9.1'), revision('fan', '0.9.3'), revision('fan-save-fix', '0.9.2', {type: 'bugfix'})];
  assert.deepEqual(run(f).changes.map(c => c.change_id), ['fan', 'fan-save-fix']);
});
test('an incomplete last revision fails instead of using an older summary', () => {
  const f = fixture(); f.manifest.changes = [revision('fan', '0.9.1'), revision('fan', '0.9.3', {summary_complete: false})];
  failure(run(f), 'INCOMPLETE_SUMMARY');
  f.manifest.merges = [{...revision('fan', '0.9.3'), from: '0.9.0', to: '0.9.3', ota_summary: words('exact combined review')}];
  assert.equal(run(f).changes[0].description, 'exact combined review');
  failure(run(f, {from: '0.9.1'}), 'INCOMPLETE_SUMMARY');
});
test('merged text must match the exact interval and requested scope', () => {
  const f = fixture(); f.manifest.changes = [revision('fan', '0.9.3')];
  f.manifest.merges = [{...revision('fan', '0.9.3', {scope: scope(['m1'])}), from: '0.9.0', to: '0.9.3', ota_summary: words('M1 only')}];
  assert.notEqual(run(f).changes[0].description, 'M1 only'); assert.equal(run(f, {model: 'm1'}).changes[0].description, 'M1 only');
  f.manifest.merges.push(copy(f.manifest.merges[0])); failure(run(f, {model: 'm1'}), 'SCOPE_CONFLICT');
});
test('new and fully withdrawn features disappear; existing users see withdrawal impact', () => {
  const f = fixture(); f.manifest.changes = [revision('fan', '0.9.1'), revision('fan', '0.9.3', {lifecycle: {state: 'withdrawn', no_residual_impact: true}, withdrawal_summary: words('removed for existing users')})];
  assert.equal(run(f).changes.length, 0);
  const result = run(f, {from: '0.9.1'}); assert.equal(result.changes[0].impact, 'withdrawn'); assert.equal(result.changes[0].description, 'removed for existing users');
});
test('withdrawal with residual impact is retained; unknown withdrawal impact fails', () => {
  const f = fixture(); const row = revision('fan', '0.9.3', {lifecycle: {state: 'withdrawn', no_residual_impact: false}});
  f.manifest.changes = [revision('fan', '0.9.1'), row]; assert.equal(run(f).changes.length, 1);
  delete row.lifecycle.no_residual_impact; failure(run(f), 'INCOMPLETE_SUMMARY');
});
test('replacement is explicit and only affects its declared model', () => {
  const f = fixture(); f.manifest.changes = [revision('old', '0.9.1'), revision('new', '0.9.3', {scope: scope(['a1']), lifecycle: {state: 'active', supersedes: ['old']}})];
  const result = run(f, {from: '0.9.1'}); assert.equal(result.changes.length, 1); assert.deepEqual(result.changes[0].replaces, ['old']); assert.equal(result.changes[0].impact, 'replaced');
  assert.equal(run(f, {from: '0.9.1', model: 'm1'}).changes.length, 0);
});
test('invalid replacement references, future references and cycles are rejected', () => {
  let f = fixture(); f.manifest.changes = [revision('new', '0.9.3', {lifecycle: {state: 'active', supersedes: ['missing']}})]; failure(run(f), 'INVALID_REPLACEMENT');
  f = fixture(); f.manifest.changes = [revision('old', '0.9.3'), revision('new', '0.9.1', {lifecycle: {state: 'active', supersedes: ['old']}})]; failure(run(f), 'INVALID_REPLACEMENT');
  f = fixture(); f.manifest.changes = [revision('a', '0.9.3', {lifecycle: {state: 'active', supersedes: ['b']}}), revision('b', '0.9.3', {lifecycle: {state: 'active', supersedes: ['a']}})]; failure(run(f), 'REPLACEMENT_CYCLE');
});
test('compatibility notices survive replacement and complete withdrawal', () => {
  const f = fixture(); f.manifest.changes = [revision('old', '0.9.1', {critical: true}), revision('new', '0.9.3', {lifecycle: {state: 'active', supersedes: ['old']}})];
  assert.deepEqual(run(f).upgrade_notices.map(n => n.change_id), ['old']);
});
test('same-version scope conflicts fail, while disjoint hardware stays isolated', () => {
  const f = fixture(); f.manifest.changes = [revision('fan', '0.9.1'), revision('fan', '0.9.1')]; failure(run(f), 'SCOPE_CONFLICT');
  f.manifest.changes = [revision('fan', '0.9.1', {scope: scope(['a1'], ['full'], ['board-a'])}), revision('fan', '0.9.1', {scope: scope(['a1'], ['full'], ['board-b'])})];
  assert.equal(run(f, {hardware: 'board-a'}).changes.length, 1); assert.equal(run(f, {hardware: 'board-c'}).changes.length, 0);
});
test('omitted hardware cannot silently drop device-specific changes', () => {
  const f = fixture(); f.manifest.changes = [revision('board-fix', '0.9.2', {scope: scope(['a1'], ['full'], ['rev-b'])})];
  failure(run(f), 'HARDWARE_REQUIRED');
  assert.equal(run(f, {hardware: 'rev-a'}).changes.length, 0);
  assert.equal(run(f, {hardware: 'rev-b'}).changes.length, 1);
});
test('numeric versions, explicit legal gaps and downgrade protection', () => {
  const f = fixture(['1.9.0', '1.10.0', '1.12.0']); f.manifest.changes = [revision('fan', '1.10.0')];
  assert.equal(run(f, {from: '1.9.0', to: '1.12.0'}).changes.length, 1);
  failure(run(f, {from: '1.12.0', to: '1.9.0'}), 'DOWNGRADE');
});
test('broken chains, unknown targets and future changes cannot produce a partial list', () => {
  const f = fixture(); f.manifest.changes = [revision('now', '0.9.2'), revision('future', '0.9.3', {review_status: 'pending'})];
  assert.deepEqual(run(f, {to: '0.9.2'}).changes.map(c => c.change_id), ['now']);
  failure(run(f, {to: '0.9.99'}), 'UNKNOWN_TARGET');
  f.manifest.releases.at(-1).previous = '0.9.99'; failure(run(f), 'BROKEN_RELEASE_CHAIN');
});
test('every intermediate firmware variant must actually exist', () => {
  const f = fixture(); f.manifest.releases[2].targets = f.manifest.releases[2].targets.filter(t => t.variant !== 'no-frontend-resources');
  failure(run(f, {variant: 'no-frontend-resources'}), 'TARGET_NOT_FOUND');
});
test('pending reviews, missing languages and unconfirmed baselines block complete output', () => {
  const f = fixture(); f.manifest.changes = [revision('fan', '0.9.1')];
  f.manifest.releases[2].coverage[0].status = 'pending'; failure(run(f), 'REVIEW_REQUIRED'); f.manifest.releases[2].coverage[0].status = 'complete';
  delete f.manifest.changes[0].description.en; failure(run(f, {locale: 'en'}), 'LANGUAGE_MISSING');
  f.manifest.releases[0].baseline_confirmed = false; failure(run(f), 'BASELINE_UNKNOWN');
});
test('branch, channel, model and variant do not cross or infer scopes', () => {
  const f = fixture(); f.manifest.changes = [revision('fan', '0.9.1', {scope: scope(['m1'], ['full'])})];
  assert.equal(run(f).changes.length, 0); assert.equal(run(f, {model: 'm1', variant: 'no-frontend-resources'}).changes.length, 0);
  failure(run(f, {channel: 'beta'}), 'UNSUPPORTED_SCOPE'); failure(run(f, {branch: 'dev'}), 'UNSUPPORTED_SCOPE'); failure(run(f, {model: 'unknown'}), 'TARGET_NOT_FOUND');
});
test('independent platform services never enter OTA differences', () => {
  const f = fixture(); f.manifest.changes = [revision('cloud', '0.9.1', {component: 'service'}), revision('page', '0.9.2', {component: 'bundled_web', scope: scope(['a1'], ['full'])})];
  assert.deepEqual(run(f).changes.map(c => c.change_id), ['page']); assert.equal(run(f, {variant: 'no-frontend-resources'}).changes.length, 0);
});
test('published suffixes must match actual firmware source, not a documentation-only tag', () => {
  const f = fixture(); const target = f.manifest.releases[0].targets.find(t => t.model === 'a1' && t.variant === 'full');
  target.source_commit = 'b'.repeat(40); target.frontend.source_commit = target.source_commit;
  f.manifest.sources[target.source_commit] = {commit: target.source_commit, frontend: copy(target.frontend)}; f.catalog.releases[0].models.a1.source_commit = target.source_commit;
  assert.equal(run(f, {from: '0.9.0-bbbbbb'}).ok, true); failure(run(f, {from: '0.9.0-abcdef'}), 'UNKNOWN_BUILD'); failure(run(f, {from: '0.9.0-bbbbbb-dirty'}), 'UNKNOWN_BUILD');
});
test('first installation and stock firmware point to migration without inventing a delta', () => {
  for (const from of ['stock', 'factory', '', null]) { const result = run(fixture(), {from}); failure(result, 'MIGRATION_REQUIRED'); assert.match(result.error.details.guide, /a1/); }
});
test('commit, path, SHA-256, size and frontend locks match archived identities', () => {
  for (const key of ['path', 'sha256', 'source_commit', 'tag_commit', 'size']) {
    const f = fixture(); const target = f.manifest.releases.at(-1).targets[0]; target[key] = key === 'size' ? 1 : 'invalid'; failure(run(f), 'ARCHIVE_MISMATCH');
  }
  const f = fixture(); f.manifest.releases.at(-1).targets[0].frontend = {kind: 'lock', sha256: 'a'.repeat(64)}; failure(run(f), 'FRONTEND_IDENTITY_MISMATCH');
});
test('repeated comparisons are pure and repeatable', () => {
  const f = fixture(); f.manifest.changes = [revision('fan', '0.9.1')]; const original = JSON.stringify(f); assert.deepEqual(run(f), run(f)); assert.equal(JSON.stringify(f), original);
});
test('the actual 23-release migration compares current archives, excludes services and preserves known gaps', () => {
  const root = path.resolve(__dirname, '..'), tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'user-notes-'));
  try {
    cp.execFileSync('python3', ['tools/release/user_notes.py', '--output', tmp], {cwd: root});
    const index = JSON.parse(fs.readFileSync(path.join(tmp, 'releases/notes/index.json')));
    const manifest = JSON.parse(fs.readFileSync(path.join(tmp, index.manifest.path)));
    const catalog = JSON.parse(fs.readFileSync(path.join(root, 'docs/release-notes/catalog.snapshot.json')));
    assert.equal(manifest.releases.length, 23); assert.equal(manifest.releases.at(-1).previous, '0.9.21');
    let result = Notes.compare(manifest, {from: '0.9.14', to: '0.9.23', model: 'a1', variant: 'full'}, catalog);
    assert.equal(result.ok, true); assert.equal(result.changes.some(c => c.component === 'service'), false);
    assert(result.changes.some(c => c.change_id === 'a1-m1-session-protocol'));
    assert.equal(result.changes.some(c => c.change_id === 'a1-fresh-air'), false);
    result = Notes.compare(manifest, {from: '0.9.3', to: '0.9.4', model: 'm1', variant: 'full'}, catalog); assert.deepEqual(result.changes, []);
    failure(Notes.compare(manifest, {from: '0.9.0', to: '0.9.23', model: 'm1', variant: 'full'}, catalog), 'REVIEW_REQUIRED');
  } finally { fs.rmSync(tmp, {recursive: true, force: true}); }
});
