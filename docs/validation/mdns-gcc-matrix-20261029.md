# mDNS compaction and A1/M1 compiler comparison, 2026-10-29

Frozen firmware source: `23d28f1d8a9315da6fb7e21d0782363982670362`.
Frozen frontend: `a1a6189877b1b310833efa83b541339b9e5f805e`, archive SHA256
`a7617da823dfed9d03dffd03934b36f7ba99b1b7061dd310107b723fe4b62b01`.
These are development candidates based on rc.3, not an rc.4 release or hardware
acceptance. This matrix covers A1 fresh-air and M1 full; TC1 was not rebuilt here.

## Change

Commit `640952cd` integrates the shared SDK mDNS assertion logger. It retains
runtime enable/disable, file/function/line/message, clock and stdio locking.
Responder, announcements, records, TTL, interfaces, reconnect and EasyLink
entry points keep their previous function bodies. The existing OOM/atomic-add
and multiple-record resume overlay remains installed first.

This removes repeated logging instructions. No discovery functionality or
mDNS task is removed; no claim is made that protocol packet construction has
been redesigned or existing SDK protocol limitations resolved. See
[the overlay analysis](mdns-compact-20261028.md).

The candidate also includes the preceding UDP metadata/schema traversal
compaction. Consequently the final size difference from an earlier candidate
must not be attributed entirely to mDNS.

## Final full object rebuilds

The flash budget is 262144 bytes. Usage below comes from each real linker map,
including initialized data stored in flash, rather than OTA container size.

| Variant | C compiler | Flash used | Headroom | Result |
| --- | --- | ---: | ---: | --- |
| A1 fresh-air | GCC 5.4.1 | 263252 B | -1108 B | Flash overflow; no OTA package |
| A1 fresh-air | GCC 14.3.1 | 260068 B | 2076 B | Build and gates passed; OTA packaged |
| M1 full | GCC 5.4.1 | 264660 B | -2516 B | Flash overflow; no OTA package |
| M1 full | GCC 14.3.1 | 260532 B | 1612 B | Build and gates passed; OTA packaged |

GCC14 saves 3184 bytes for A1 and 4128 bytes for M1 against the same source's
GCC5 build. Both GCC14 trials use **GCC5 linker and runtime libraries** for SDK
compatibility. They are not a GCC14 runtime migration. Compiler dispatch logs
record 138 A1 and 142 M1 compile invocations per cell, exclusively using the
requested compiler version. Actual `.comment` sections and matching compile
invocations were checked for application main/service, shared HTTP status and
MQTT service objects.

Each final attempt removed every workspace intermediate object before compiling.
Pinned, patched SDK source was retained. Prior logs/results and compressed
workspace intermediates were preserved. Build-host disk exhaustion interrupted
some intermediate attempts; those attempts are not accepted results. Verified
SDK archives for the two completed A1 cells freed space before the final M1
rebuilds. The final four results all identify the frozen source above.

## Isolated mDNS saving

Independent relinks replaced only the SDK mDNS object and retained all other
archives/options. This measurement was made at `a3ea127f`, before the packaging
metadata correction in `23d28f1d`.

| Variant/compiler | Without logger sharing | With logger sharing | Saving |
| --- | ---: | ---: | ---: |
| A1/GCC5 | 263588 B | 263252 B | 336 B |
| A1/GCC14 | 260372 B | 260068 B | 304 B |
| M1/GCC5 | 264996 B | 264660 B | 336 B |
| M1/GCC14 | 260836 B | 260532 B | 304 B |

## Verification

23 host regression tests passed, including ASAN/UBSAN mDNS logging parity,
overlay order/idempotence, unchanged protocol function bodies, OOM/thread-create
failure, atomic record ownership, multi-record resume, TXT canaries, v4 envelope
validation, typed diagnostics parity and stack-gate negative cases.

The first GCC14 pass exposed missing SDK SHAUtils stack files in A1/M1.
Commit `a3ea127f` adds global stack-usage collection, matching TC1. The negative
gate still rejects missing/dynamic frames; task budgets and reserves remain
unchanged. The final GCC14 builds passed button/worker/image, HTTP/MQTT and LAN
stack checks and the absence-of-legacy-JSON-object/parser check.

Worst reported paths below include the gate's fixed SDK reserve. They are static
bounds, not physical RTOS watermarks.

| GCC14 variant | HTTP / 12288 B | MQTT / 16384 B | LAN / 12288 B |
| --- | ---: | ---: | ---: |
| A1 fresh-air | 7708 B | 10248 B | 7868 B |
| M1 full | 8516 B | 12432 B | 8276 B |

Commit `23d28f1d` fixes package compiler metadata previously hardcoded to GCC5.
Both final GCC14 package manifests identify GCC14 and the correct clean source
commit. All manifest file hashes and `SHA256SUMS` were checked against disk.

| GCC14 unsigned OTA | Bytes | SHA256 |
| --- | ---: | --- |
| A1 fresh-air | 739316 | `946a80cefcc1bd3bc9b3ab3c1bd4c403e016d4761b7f46687f1a1f93a53764a9` |
| M1 full | 739780 | `32da52ebd0e3b0dadfce483a21f1abff4330d6c631eb0aac6fd4d9ad25e4417f` |

Local evidence root:
`/home/fjr/hobbies/phicomm-messagepack-lab/private/mdns-rebuild-20261029-640952cd/`.
`verified-summary.json` is the final inventory; `results-final.json`, per-cell
`result.json`, compiler dispatch, stack files, maps, build logs and packages
retain the underlying evidence. `mdns-delta.json` records isolated relinks.
The initial `matrix-inventory.json` belongs to the earlier attempt, not the
final accepted build inventory.

No device was flashed in this task. Physical discovery, linkage, busy/P95,
recovery time and stability acceptance remain outstanding. Successful linking
and host regression results do not establish those outcomes.
