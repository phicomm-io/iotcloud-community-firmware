# Firmware release gates

CI builds the real full and hub DAIKIN images and checks the application binary
and ELF before collecting release assets. `check_esp_budget.py --layout stock`
requires the application plus 32 KiB of headroom to fit the original **0x180000
(1,572,864 byte)** OTA slot. A stock application migration keeps the bootloader
and partition table, so this limit remains for later community OTA updates.

The repository's `DAIKIN/partitions.csv` describes the **wired installation**
layout, with **0x1e0000 (1,966,080 byte)** OTA slots. Passing only
`--layout wired` does not establish compatibility with a device that retained
the stock table. The tool never writes a partition table or changes a device.
Its JSON report includes both capacities and both compatibility results, the
selected gate, the measured binary/ELF hashes and the static RAM budget. Failed
size gates also produce a report; CI preserves that report and stops signing.

The ten current build combinations are TC1 full/slim, M1 full/slim, A1
full/slim/fresh-air/fresh-air-slim, and DAIKIN full/hub. `products.py --targets`
is the shared enumeration for building the signing/catalog matrix. DAIKIN v1
catalog entries authenticate each signed payload's exact full/hub image label
and require the downloadable raw application to be the same payload.

Stable and RC v1+ tags at a clean, exact commit on `main` may publish firmware. Branch builds
and a manual signing dry run do not promote an OTA manifest. New manifests use
`/ota/v1/<model>/<variant>/latest.json`; legacy paths stop at the v1.0.0 bridge.
The v0.x archive retains its original asset and metadata shape.

`VERSION` contains the numeric core, for example `1.0.0`. Candidate tags are
`v1.0.0-rc` or `v1.0.0-rc.N`, where N is a nonnegative integer without leading
zeros. The ordering is `rc < rc.0 < rc.1 < stable`; commit suffixes are build
identities, not release tags. The tagged core must match `VERSION`.

Candidates are GitHub prereleases with `--latest=false`. They have immutable
R2 assets under `/releases/<tag>/` and a separate `/releases/candidates.json`
catalog. The current production Worker serves stable releases, so candidate
assets are verified by authenticated API readback; public mirror verification
remains a stable-release gate. Use GitHub prereleases or a separately published
Pages candidate archive for manual downloads. Stage with `publish.py DIRECTORY`,
make the GitHub prerelease public, then use `publish.py DIRECTORY --candidate`.
`--promote` rejects candidates before any remote operation. Candidate workflows
do not deploy the OTA Worker, publish Pages, replace `stable.json`, create the
v1.0.0 bridge, or change legacy/v1 device `latest.json` manifests.

For local note validation or a Pages candidate archive, explicitly run
`archive_pages.py OUTPUT --include-candidates --candidates-r2`. This reads the
candidate catalog and assets through the authenticated API, verifies file
hashes, and retains them beside the unchanged stable `releases/index.json`.
Only the temporary note-validation catalog combines both channels. Without
`--candidates-r2`, candidate reads use the selected public archive source;
only a 404 for the candidate catalog is accepted as an empty first catalog.
Use `import_user_notes.py --archive OUTPUT --include-candidates --previous VERSION`
to import one new candidate's immutable originals with an explicit reviewed
baseline. Candidate changes and coverage use the `candidate` channel; stable
release chains continue to follow stable predecessors.

Generate the optional Pages comparison snapshot with
`user_notes.py --output OUTPUT --archive OUTPUT --formal --include-candidates`.
Its pointer is `/releases/notes/candidate/index.json`, marked with candidate
channel/source scope. It references independent hash-named catalog and manifest
files in the same candidate directory. The combined catalog keeps its stable
selection; neither `releases/index.json` nor the default note pointer is changed.
`--verify-output OUTPUT --formal --include-candidates` verifies that independent
snapshot. These files are for the local Pages archive; the stable mirror-note
publisher does not publish the candidate pointer.

## Dual-format device metadata

New catalogs produce `<asset-stem>.latest.json` and
`<asset-stem>.latest.msgpack` together in the immutable release directory.
`release.json.device_manifests["<model>/<variant>"]` indexes both files by
`path`, byte `size`, and `sha256`. The ten firmware variants, signed firmware
layout, and firmware signature verification are unchanged. `SHA256SUMS` includes
both metadata formats; metadata is not itself a new signed firmware envelope.
Retries reject different manifest bytes under the same immutable name.
Historical immutable release directories are not regenerated or modified.

Both files have the same flat schema-1 document: string fields `tag`, `version`,
`commit`, `model`, `firmware_variant`, `path`, `sha256`, `release_log`, plus
unsigned integer fields `schema` (1) and `size` (signed image length). Strings
use UTF-8; MP uses standard string, map, and unsigned integer wire types. There
are no extension types, floats, or numbered fields. Both documents must fit
4096 bytes. Publication readback and archive validation compare both files to
the canonical document, not only to their self-reported hashes.

The stable Worker can serve `/ota/v1/<model>/<variant>/latest.msgpack` from
the indexed immutable asset selected by `stable.json`; older stable releases
without an indexed binary manifest return 404. JSON and legacy bridge routes
retain their existing behavior. The Worker continues to reject candidate URLs.
When explicitly generating a candidate Pages archive, separate
`/ota/candidate/<model>/<variant>/latest.json` and `latest.msgpack` pointers
select the highest candidate version. Removing a candidate catalog also removes
these candidate pointers. These files never select or overwrite stable/legacy
pointers. Generating them locally does not publish a site or deploy a Worker.
