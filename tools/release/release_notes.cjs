#!/usr/bin/env node
'use strict';
const fs = require('node:fs');
const Notes = require('../../site/release-notes/core.js');
const options = {};
for (let i = 2; i < process.argv.length; i += 2) {
  const flag = process.argv[i], value = process.argv[i + 1];
  if (!flag.startsWith('--') || !value || Object.hasOwn(options, flag.slice(2))) {
    process.stderr.write('Use --manifest FILE --catalog FILE --from VERSION --to VERSION --model MODEL --variant VARIANT [--locale zh-CN|en] [--format json|text]\n');
    process.exit(2);
  }
  options[flag.slice(2)] = value;
}
try {
  const {manifest, catalog, format = 'json', ...query} = options;
  if (!['json', 'text'].includes(format)) throw Error('Unknown output format.');
  const result = Notes.compare(JSON.parse(fs.readFileSync(manifest)), query, JSON.parse(fs.readFileSync(catalog)));
  if (format === 'json') process.stdout.write(JSON.stringify(result, null, 2) + '\n');
  else if (!result.ok) process.stdout.write(result.error.code + ': ' + result.error.reason + '\n');
  else {
    process.stdout.write(`${result.from} → ${result.to} · ${result.scope.model} · ${result.scope.variant}\n`);
    for (const change of result.changes) process.stdout.write(`[${change.type}] ${change.title}\n${change.description}\n\n`);
    for (const notice of result.upgrade_notices) process.stdout.write(`! ${notice.title}: ${notice.description}\n`);
  }
  if (!result.ok) process.exitCode = 1;
} catch (error) {
  process.stderr.write('Unable to export release notes: ' + error.message + '\n'); process.exitCode = 2;
}
