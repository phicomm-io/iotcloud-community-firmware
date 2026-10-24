# Structured release notes

The public website computes upgrade differences entirely in the browser. Devices
keep their existing OTA protocol and per-release notes. The firmware does not
fetch this manifest, retain history, or aggregate changes. Static refactoring does
not change firmware inputs; the separate v0.9.24 frontend entry links to this site.

## Sources and review

`catalog.snapshot.json` pins the 23 published archives through v0.9.23 (v0.9.22
was not archived). `archive-identities.json` pins their actual firmware sources,
frontend locks and original-note hashes. `release-metadata.json` retains published
release metadata. `sources/` retains the original Markdown; `archived-wiki/`
retains the frozen Wiki snapshot. Never rewrite these originals.

`reviews.json` associates each deterministic source block with its full source
hash, decision, evidence and change references. `changes.json` contains reviewed
Chinese and English prose. Approval is separate from lifecycle. IDs are stable;
independent bug fixes keep separate IDs. Components are `firmware`, `bundled_web`
and `service`. Only the first two enter firmware comparisons. Scope always lists
models, variants, hardware, branches and channels explicitly.

Current migration: 142 blocks, 93 approved, 47 excluded, 2 pending; 81 reviewed
change revisions. v0.9.0 only has a commit link. v0.9.5 contains one unclear TMCP
and binding statement. Their comparisons are blocked. v0.9.4 reuses the previous
implementation and adds no feature. Pending source text and internal evidence
are never copied into the public manifest. See [pending-review.md](pending-review.md).

```sh
python3 tools/release/import_user_notes.py --archive /path/to/retained-site
python3 tools/release/user_notes.py --migrate
python3 tools/release/user_notes.py --pending
# Exactly one new archive requires its explicit predecessor:
python3 tools/release/import_user_notes.py --archive /path/to/prepared-site --previous 0.9.23
```

Import is idempotent and refuses changed originals. Migration invalidates all
reviews for a version when its original bytes change. Review any new records,
add bilingual changes and their proof references, then compile. A reviewed
baseline means the authored change IDs have a known state at that release; it
never invents descriptions for missing historical material. A pending source
version is not a permitted comparison endpoint or intermediate release.

## Public resources and integrity

`/releases/notes/index.json` points to
`/releases/notes/manifest.<sha256>.json` and the unchanged
`/releases/index.json`. Both descriptors contain path, size and SHA-256. The
manifest also pins the catalog hash. The browser verifies both responses and
freezes the snapshot for one query. A mismatched mirror fails without mixing
sources. The hash verifies snapshot consistency; HTTPS supplies transport trust.
The public JSON shape is described in [manifest.schema.json](manifest.schema.json).

Publication prepares the actual archive, verifies signatures/commit/variant/path/
hash and historical frontend locks, compiles notes, generates both languages,
then checks the final output. Resources and the last-written index are published
in one Git commit. Existing hashed manifests, original Markdown, firmware assets,
legacy OTA fields and URLs are retained. The existing `releases/` mirror allowlist
already covers these resources. A local build can use the fixed metadata snapshot;
a formal publication always requires actual firmware files and historical Git
objects (`fetch-depth: 0`). No firmware is built by these tools.

```sh
python3 site/build.py build/pages                       # local fixed snapshot
python3 site/build.py build/pages --archive /path/to/site --formal
PHICOMM_PAGES_ARCHIVE=/path/to/site tools/release/pages.sh --build-only
```

## Browser, Node and integration

Load `/releases/notes/core.js` for the pure `ReleaseNotes` API. `client.js` supplies
optional static-resource fetching and one-shot device reading. Neither module
uses MQTT, storage, OTA checks or installation. Node and the CLI use exactly the
same comparison implementation:

```js
const ReleaseNotes = require('./site/release-notes/core.js');
const result = ReleaseNotes.compare(manifest, {
  from: '0.9.14', to: '0.9.23', model: 'a1', variant: 'full',
  branch: 'main', channel: 'stable', locale: 'zh-CN'
}, catalog);
if (!result.ok) console.error(result.error.code, result.error.reason);
else console.log(result.groups, result.upgrade_notices, result.target);
```

Success returns normalized versions, scope, target identity, categorized changes
and compatibility notices. Failure returns only `{ok:false,error:{code,reason,
details?}}`, never a partial changes list. The interval is `(from,to]` along the
explicit release chain. Same-version queries validate both identities and return
empty. Known source suffixes must match the archived source; dirty and unknown
development builds are rejected without stripping their suffix.

`ota_summary` is the full reviewed state of an ID at its revision, not just the
latest incremental edit. Incomplete revisions block output unless an approved
merge exactly matches the interval and scope. `withdrawn` records must state
`no_residual_impact`; withdrawal of an existing feature remains visible.
`superseded` records name `superseded_by`, or the new revision lists `supersedes`.
Only explicitly scoped relationships merge, and compatibility notices survive.
If a scope has hardware-specific notes, callers must provide `hardware`.

Stable errors include `MIGRATION_REQUIRED`, `UNKNOWN_BUILD`, `UNKNOWN_SOURCE`,
`UNKNOWN_TARGET`, `DOWNGRADE`, `TARGET_NOT_FOUND`, `UNSUPPORTED_SCOPE`,
`HARDWARE_REQUIRED`, `BROKEN_RELEASE_CHAIN`, `REVIEW_REQUIRED`, `LANGUAGE_MISSING`,
`BASELINE_UNKNOWN`, `INCOMPLETE_SUMMARY`, `SCOPE_CONFLICT`, `INVALID_REPLACEMENT`,
`REPLACEMENT_CYCLE`, `ARCHIVE_MISMATCH`, `FRONTEND_IDENTITY_MISMATCH`.
Fetch errors include `SOURCE_MISMATCH`, `TIMEOUT`, `REQUEST_ABORTED`,
`DEVICE_UNAUTHORIZED`, `OLD_DEVICE_API`, `DEVICE_BUSY`, `LOCAL_NETWORK_DENIED`,
`NETWORK_BLOCKED` and `DEVICE_IDENTITY_INCOMPLETE`. Generic browser network errors
cannot distinguish CORS from local-network permission; the UI says so.

```sh
python3 tools/release/user_notes.py --output build/pages
node tools/release/release_notes.cjs --manifest build/pages/releases/notes/manifest.HASH.json \
  --catalog docs/release-notes/catalog.snapshot.json --from 0.9.14 --to 0.9.23 \
  --model a1 --variant full --locale en --format text
```

The optional device reader makes one GET to existing `/api/v1/info`. It accepts
only `data.model`, `data.firmware_variant`, and real `data.version`. Protocol v1/v2
is never a firmware version. The management password exists only in page memory,
is cleared from the input as the request starts, and is never persisted. Manual
selection remains available after old APIs, 401, busy, timeout or CORS failures.
Source/condition changes invalidate old replies. Share URLs contain only `from`,
`to`, `model` and `variant`; they never contain addresses, passwords or MACs.
For local-console or IotCloud integration, reuse the pure API or open the existing
Chinese `/guides/release-log.html` or English `/guides/en/release-log.html` with
these four query parameters. Do not pass authentication or device addresses.

## Validation

```sh
python3 tests/test_user_notes.py
node --test tests/test_release_notes.cjs tests/test_release_notes_web.cjs
python3 tests/test_site_docs.py
python3 tests/test_archive_pages.py
# Serve the built site at 127.0.0.1:18765, then:
node tests/test_release_notes_browser.cjs
```

See [validation.md](validation.md) for the recorded simulation, browser, old-device
and publication boundaries. The ordinary firmware CI remains separate.
