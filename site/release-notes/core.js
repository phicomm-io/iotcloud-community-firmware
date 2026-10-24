/* Shared by the static website, Node.js and the export CLI. No I/O or device state. */
(function (root, factory) {
  'use strict';
  if (typeof module === 'object' && module.exports) module.exports = factory();
  else root.ReleaseNotes = factory();
}(typeof globalThis !== 'undefined' ? globalThis : this, function () {
  'use strict';
  const TYPES = ['feature', 'improvement', 'bugfix', 'security', 'breaking'];
  const SHA = /^[a-f0-9]{64}$/, COMMIT = /^[a-f0-9]{40}$/;
  const clone = value => JSON.parse(JSON.stringify(value));
  class NotesError extends Error {
    constructor(code, reason, details) { super(reason); this.code = code; this.details = details; }
  }
  function need(condition, code, reason, details) {
    if (!condition) throw new NotesError(code, reason, details);
  }
  function parse(value) {
    need(typeof value === 'string', 'INVALID_VERSION', 'A firmware version is required.');
    const match = /^v?(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)(?:-([a-f0-9]{6,40}))?$/.exec(value);
    need(match && !value.includes('dirty'), 'UNKNOWN_BUILD', 'Unknown development or dirty builds cannot be treated as a published release.');
    const numbers = match.slice(1, 4).map(Number);
    need(numbers.every(Number.isSafeInteger), 'INVALID_VERSION', 'Version components exceed the supported numeric range.');
    return {version: numbers.join('.'), numbers, suffix: match[4] || null};
  }
  function order(a, b) {
    const x = parse(a).numbers, y = parse(b).numbers;
    for (let i = 0; i < 3; i++) if (x[i] !== y[i]) return x[i] < y[i] ? -1 : 1;
    return 0;
  }
  function scopeMatches(scope, query) {
    return ['models', 'variants', 'branches', 'channels', 'hardware'].every(key => {
      const single = {models: 'model', variants: 'variant', branches: 'branch', channels: 'channel', hardware: 'hardware'}[key];
      const values = scope && scope[key];
      return Array.isArray(values) && values.length > 0 && (values.includes('*') || values.includes(query[single]));
    });
  }
  function text(value, locale, label) {
    need(value && typeof value[locale] === 'string' && value[locale].trim(), 'LANGUAGE_MISSING', 'A reviewed translation is missing: ' + label);
    return value[locale];
  }
  function stable(value) {
    if (Array.isArray(value)) return '[' + value.map(stable).join(',') + ']';
    if (value && typeof value === 'object') return '{' + Object.keys(value).sort().map(k => JSON.stringify(k) + ':' + stable(value[k])).join(',') + '}';
    return JSON.stringify(value);
  }
  function entryFor(release, query) {
    const model = release && release.models && release.models[query.model];
    return query.variant === 'full' ? model : model && model.variants && model.variants[query.variant];
  }
  function identity(release, query, catalog, manifest) {
    const matches = release.targets.filter(t => t.model === query.model && t.variant === query.variant &&
      (t.hardware === '*' || t.hardware === query.hardware));
    need(matches.length === 1, matches.length ? 'SCOPE_CONFLICT' : 'TARGET_NOT_FOUND', 'This release has no unique archived firmware for the requested device scope.');
    const target = matches[0];
    const archived = catalog.releases.filter(r => parse(r.version || r.tag).version === release.version);
    need(archived.length === 1, 'ARCHIVE_MISMATCH', 'The release is missing or duplicated in the archive catalog.');
    const record = archived[0], entry = entryFor(record, query);
    need(entry, 'TARGET_NOT_FOUND', 'The requested firmware variant does not exist in the archive.');
    need((record.branch || 'main') === query.branch && (record.channel || 'stable') === query.channel,
      'ARCHIVE_MISMATCH', 'Archive branch or channel differs from the requested scope.');
    const asset = entry.downloads && entry.downloads[target.kind];
    const source = entry.source_commit || record.models[query.model].source_commit || record.commit;
    need(COMMIT.test(target.tag_commit) && COMMIT.test(target.source_commit) &&
      record.commit === target.tag_commit && source === target.source_commit && asset &&
      asset.path === target.path && asset.sha256 === target.sha256 && asset.size === target.size &&
      SHA.test(target.sha256) && Number.isSafeInteger(target.size) && target.size > 0 &&
      target.path.startsWith('/releases/v' + release.version + '/') && !target.path.includes('..'),
      'ARCHIVE_MISMATCH', 'Target commit, firmware path, size or SHA-256 differs from the actual archive.');
    const proof = manifest.sources && manifest.sources[source];
    need(proof && proof.commit === source && stable(proof.frontend) === stable(target.frontend),
      'FRONTEND_IDENTITY_MISMATCH', 'The archived frontend identity cannot be confirmed.');
    const frontend = proof.frontend;
    need(frontend && ((frontend.kind === 'in-tree' && frontend.source_commit === source) ||
      (frontend.kind === 'lock' && SHA.test(frontend.sha256) && frontend.lock && COMMIT.test(frontend.lock.commit) && SHA.test(frontend.lock.sha256))),
      'FRONTEND_IDENTITY_MISMATCH', 'The source does not have a valid archived frontend lock or in-tree identity.');
    return target;
  }
  function published(input, release, target) {
    need(!input.suffix || target.source_commit.startsWith(input.suffix), 'UNKNOWN_BUILD',
      'The commit suffix does not identify the archived source used for this firmware.');
    need(input.version === release.version, 'UNKNOWN_VERSION', 'Release identity does not match.');
  }
  function compare(manifest, options, catalog) {
    try {
      const query = Object.assign({branch: 'main', channel: 'stable', locale: 'zh-CN', hardware: '*'}, options);
      const migration = query.from == null || ['', 'stock', 'factory', 'initial'].includes(query.from);
      if (migration) throw new NotesError('MIGRATION_REQUIRED', 'Use the installation or stock-migration guide instead of a community-firmware difference.',
        {guide: query.model === 'm1' ? '/guides/m1-no-disassembly.html' : query.model === 'a1' ? '/guides/a1-no-disassembly.html' : '/guides/quick-start.html'});
      need(manifest && manifest.schema === 1 && catalog && catalog.schema === 1 &&
        Array.isArray(manifest.releases) && Array.isArray(manifest.changes) && Array.isArray(catalog.releases),
        'INVALID_DATA', 'Unsupported or incomplete release-note data.');
      need(manifest.supported_scopes.some(s => s.branch === query.branch && s.channel === query.channel),
        'UNSUPPORTED_SCOPE', 'No reviewed data exists for this branch and channel.');
      need(['zh-CN', 'en'].includes(query.locale), 'LANGUAGE_MISSING', 'This language has not been reviewed.');
      need(typeof query.model === 'string' && typeof query.variant === 'string', 'SCOPE_REQUIRED', 'Select a model and firmware variant.');
      const from = parse(query.from), to = parse(query.to);
      need(order(from.version, to.version) <= 0, 'DOWNGRADE', 'Downgrades do not have an upgrade difference.');
      const releases = manifest.releases.filter(r => r.branch === query.branch && r.channel === query.channel);
      const byVersion = new Map();
      for (const r of releases) {
        need(!byVersion.has(r.version) && parse(r.version).version === r.version && Array.isArray(r.targets), 'INVALID_DATA', 'Duplicate or malformed release record.');
        byVersion.set(r.version, r);
      }
      need(byVersion.has(from.version), 'UNKNOWN_SOURCE', 'The starting release is not in the published archive.');
      need(byVersion.has(to.version), 'UNKNOWN_TARGET', 'The target release is not in the published archive.');
      const start = byVersion.get(from.version), finish = byVersion.get(to.version);
      const target = identity(finish, query, catalog, manifest), initialTarget = identity(start, query, catalog, manifest);
      published(from, start, initialTarget); published(to, finish, target);
      const result = {ok: true, from: from.version, to: to.version,
        scope: {model: query.model, variant: query.variant, hardware: query.hardware, branch: query.branch, channel: query.channel},
        locale: query.locale, target: clone(target), changes: [], groups: Object.fromEntries(TYPES.map(t => [t, []])), upgrade_notices: []};
      if (from.version === to.version) return result;
      const path = [], seen = new Set(); let cursor = finish;
      while (cursor) {
        need(!seen.has(cursor.version), 'BROKEN_RELEASE_CHAIN', 'The release chain contains a cycle.');
        seen.add(cursor.version); path.push(cursor);
        if (cursor.previous === null) break;
        need(byVersion.has(cursor.previous) && order(cursor.previous, cursor.version) < 0,
          'BROKEN_RELEASE_CHAIN', 'An explicit previous release is missing or out of order.');
        cursor = byVersion.get(cursor.previous);
      }
      need(seen.has(from.version), 'BROKEN_RELEASE_CHAIN', 'The starting release is not on the target release chain.');
      const interval = path.filter(r => order(r.version, from.version) > 0);
      for (const release of [start, ...interval]) {
        identity(release, query, catalog, manifest);
        const coverage = release.coverage.filter(c => scopeMatches(c.scope, query));
        need(coverage.length === 1, 'COVERAGE_UNKNOWN', 'Coverage for this release and scope is not confirmed.');
        need(coverage[0].status === 'complete', 'REVIEW_REQUIRED', 'Relevant release records still need review.', {version: release.version});
        need(coverage[0].locales.includes(query.locale), 'LANGUAGE_MISSING', 'This release has an incomplete translation.', {version: release.version});
      }
      need(start.baseline_confirmed === true, 'BASELINE_UNKNOWN', 'The starting state cannot be confirmed.');
      if (query.hardware === '*') need(!manifest.changes.some(c => seen.has(c.version) && c.component !== 'service' &&
        scopeMatches(Object.assign({}, c.scope, {hardware: ['*']}), query) && !c.scope.hardware.includes('*')),
        'HARDWARE_REQUIRED', 'This device scope has hardware-specific changes; select its hardware revision.');
      const revisions = manifest.changes.filter(c => seen.has(c.version) && scopeMatches(c.scope, query) && c.component !== 'service');
      const histories = new Map();
      for (const row of revisions) {
        need(row.review_status === 'approved', 'REVIEW_REQUIRED', 'A relevant change has not been approved.');
        need(TYPES.includes(row.type) && ['firmware', 'bundled_web'].includes(row.component) &&
          row.lifecycle && ['active', 'withdrawn', 'superseded'].includes(row.lifecycle.state), 'INVALID_DATA', 'Malformed change record.');
        need(typeof row.change_id === 'string' && /^[a-z0-9][a-z0-9._-]*$/.test(row.change_id), 'INVALID_DATA', 'Malformed stable change ID.');
        const rows = histories.get(row.change_id) || [];
        need(!rows.some(c => c.version === row.version), 'SCOPE_CONFLICT', 'Same-version change revisions overlap in this device scope.');
        rows.push(row); histories.set(row.change_id, rows);
        if (order(row.version, from.version) > 0) {
          text(row.title, query.locale, row.change_id); text(row.description, query.locale, row.change_id);
        }
      }
      for (const rows of histories.values()) rows.sort((a, b) => order(a.version, b.version));
      const edges = new Map(), edgeEvents = [];
      function edge(old, next, row) {
        need(old !== next && histories.has(old) && histories.has(next) &&
          histories.get(old).some(r => order(r.version, row.version) <= 0) &&
          histories.get(next).some(r => order(r.version, row.version) <= 0),
          'INVALID_REPLACEMENT', 'A replacement points to an unknown, future or inapplicable change.');
        const children = edges.get(old) || new Set(); children.add(next); edges.set(old, children);
        edgeEvents.push({old, next, version: row.version});
      }
      for (const row of revisions) {
        for (const old of row.lifecycle.supersedes || []) edge(old, row.change_id, row);
        if (row.lifecycle.state === 'superseded') {
          need(typeof row.lifecycle.superseded_by === 'string', 'INVALID_REPLACEMENT', 'Superseded changes must name their replacement.');
          edge(row.change_id, row.lifecycle.superseded_by, row);
        }
      }
      const visiting = new Set(), visited = new Set();
      function visit(id) {
        need(!visiting.has(id), 'REPLACEMENT_CYCLE', 'Replacement relationships contain a cycle.');
        if (visited.has(id)) return;
        visiting.add(id); for (const next of edges.get(id) || []) visit(next); visiting.delete(id); visited.add(id);
      }
      for (const id of histories.keys()) visit(id);
      function stateAt(limit) {
        const states = new Map();
        for (const [id, rows] of histories) {
          const applicable = rows.filter(r => order(r.version, limit) <= 0);
          if (applicable.length) states.set(id, {row: applicable.at(-1), state: applicable.at(-1).lifecycle.state});
        }
        for (const e of edgeEvents) if (order(e.version, limit) <= 0) {
          const state = states.get(e.old), replacement = states.get(e.next);
          if (state && replacement && replacement.state === 'active' && order(state.row.version, e.version) <= 0) {
            state.state = 'superseded'; state.replacement = e.next;
          }
        }
        return states;
      }
      const before = stateAt(from.version), after = stateAt(to.version);
      for (const [id, state] of after) {
        const old = before.get(id), row = state.row;
        if (order(row.version, from.version) <= 0 && state.state === (old && old.state)) continue;
        if (state.state === 'superseded') continue;
        if (state.state === 'withdrawn' && (!old || old.state !== 'active') && row.lifecycle.no_residual_impact === true) continue;
        if (state.state === 'withdrawn') need(typeof row.lifecycle.no_residual_impact === 'boolean', 'INCOMPLETE_SUMMARY', 'Withdrawal impact must be explicitly reviewed.');
        const merges = (manifest.merges || []).filter(m => m.change_id === id && m.from === from.version && m.to === to.version && scopeMatches(m.scope, query));
        need(merges.length <= 1, 'SCOPE_CONFLICT', 'More than one exact interval summary applies.');
        const selected = merges[0] || row;
        need(selected.review_status === 'approved', 'REVIEW_REQUIRED', 'The exact interval summary needs review.');
        need(selected.summary_complete === true, 'INCOMPLETE_SUMMARY', 'The latest revision has no complete reviewed OTA summary.');
        let description;
        if (state.state === 'withdrawn') description = text(selected.withdrawal_summary || selected.ota_summary, query.locale, id);
        else description = text(selected.ota_summary, query.locale, id);
        const replaces = edgeEvents.filter(e => e.next === id && order(e.version, from.version) > 0 && before.get(e.old)?.state === 'active').map(e => e.old);
        const impact = state.state === 'withdrawn' ? 'withdrawn' : replaces.length ? 'replaced' : old && old.state === 'active' ? 'updated' : 'added';
        const type = impact === 'withdrawn' ? 'breaking' : row.type === 'feature' && impact !== 'added' ? 'improvement' : row.type;
        const item = {change_id: id, version: row.version, type, component: row.component, impact,
          title: text(selected.title || row.title, query.locale, id), description, scope: clone(row.scope), replaces};
        result.changes.push(item); result.groups[type].push(item);
      }
      for (const row of revisions) if (row.critical && order(row.version, from.version) > 0) {
        result.upgrade_notices.push({change_id: row.change_id, version: row.version,
          title: text(row.title, query.locale, row.change_id), description: text(row.description, query.locale, row.change_id)});
      }
      result.changes.sort((a, b) => TYPES.indexOf(a.type) - TYPES.indexOf(b.type) || order(a.version, b.version) || a.change_id.localeCompare(b.change_id));
      for (const type of TYPES) result.groups[type] = result.changes.filter(c => c.type === type);
      return result;
    } catch (error) {
      if (!(error instanceof NotesError)) return {ok: false, error: {code: 'INVALID_DATA', reason: 'Malformed or incomplete release-note data.'}};
      const result = {ok: false, error: {code: error.code, reason: error.message}};
      if (error.details) result.error.details = clone(error.details);
      return result;
    }
  }
  return Object.freeze({compare, parseVersion: parse, compareVersions: order, scopeMatches, types: Object.freeze(TYPES.slice())});
}));
