# M1 configuration recovery hotfix for rc.2

## Report and scope

The M1 report concerns configuration recovery after upgrading from an unidentified
third-party firmware. The original image/version and a Flash dump are unavailable.
An unsupported application layout intentionally preserves its stored bytes and
requires explicit consent to clear them. This hotfix does not infer a migration
format from that report or disable the storage protection.

The separate A1 report concerns the Wi-Fi touch key: it flashes during the first
two seconds, then holding for ten seconds and releasing does not restart the
device or open its AP. A1 gesture code is unchanged. Its physical cause remains
unconfirmed; collect `panel_raw`, `panel_errors` and `panel_last_error` during a
hold/release before selecting a gesture or I2C fix.

## Confirmed software defects and changes

- M1's network-only reset could treat the SDK recovery guard's no-write return
  as success, schedule a reboot and acknowledge the controller without a
  persistent reset. Reject this operation under the configuration mutex when
  either layout recovery or commit uncertainty is active. An explicitly confirmed
  full clear retains the existing transactional storage and uncertainty checks.
- Full and minimal setup pages cached the startup recovery state. Re-read
  capabilities and migration state during recovery, after an explicit clear and
  on a full refresh. Retire reads started before the clear and wait for a fresh
  state without a pending reboot before reporting completion. A restart with
  recovery still active is unconfirmed and retryable.
- The full maintenance page returned a DOM node, while its caller mounted `el`.
  Return the page object so reboot, backup and reset controls are actually shown.
- Reset failures remain visible and retryable. Accepted requests disable duplicate
  clearing. Confirmed clearing refreshes cached form defaults; normal polling
  preserves unsaved input. Both English and Chinese explain the restart wait.

No public command shape, persisted layout, schema version or MCU firmware changes.
The frontend code is shared across products; firmware publication still builds
the repository's complete ten-variant matrix.

The public download pages show only the latest RC and label RC builds as Beta,
with major future changes expected and use not recommended. Historical artifacts
and release-chain records remain intact. Older target links require the user to
confirm the latest target; an installed older RC can still be compared as a source.

## Local verification (2026-10-27)

| Verification | Result | Evidence and boundary |
| --- | --- | --- |
| Rendered configuration recovery pages | PASS | `tests/test_config_recovery.cjs` in the frontend: 13 scenarios each against source and minified pages; confirmation/cancellation, failed writes, duplicate prevention, pending reboot, disconnect/reconnect, same-tab setup, external clear, stale reads, full-status automatic recovery polling, persistent recovery and fresh setup on all four models |
| Existing full and minimal pages | PASS | Frontend `test_web.cjs` for M1/A1/TC1 and `test_setup.cjs` |
| Frontend unit, Python and syntax checks | PASS | 78 frontend unit tests, 7 Python tests and four-product syntax validation; frontend source and archive identities are pinned in `frontend.lock.json` |
| Frontend hotfix CI | PASS | [Branch run](https://github.com/phicomm-io/iotcloud-community-frontend/actions/runs/40499404791) and [PR run](https://github.com/phicomm-io/iotcloud-community-frontend/actions/runs/40499411002), including Chromium/WebKit and compressed-page tests; [PR 31](https://github.com/phicomm-io/iotcloud-community-frontend/pull/31) merged as `3002c64c3136a59a7f9692a4a57f8e5fe455d9af` |
| Frontend release reuse | PASS | Published the unmodified successful branch-CI archive for `6a92cc6ef7f8608937d3229d696ffb5f0f930679`, an ancestor of main with an identical source tree to its merge commit. Downloaded and verified the published archive against the pinned SHA-256. Redundant main CI was cancelled with the user's authorization |
| Latest Beta display | PASS | 60 JavaScript comparison/client/page tests; Chromium: four bilingual desktop/mobile scenarios plus two no-JavaScript scenarios. Latest numeric RC only, old target shares fail closed, old installed versions remain valid sources, stable history is preserved |
| Production M1 reset/UART/boot + pinned SDK | PASS | `tests.test_m1_config_recovery`: actual product defaults/reset/frame handling and SDK CRC, commit and boot-read functions; ASan/UBSan; test Flash partitions, no hardware |
| Baseline negative control | PASS | The new M1 regression fails on the unpatched main source at `!m1_reset(false)`, detecting the false success |
| Existing storage/boot regressions | PASS | `tests.test_config_commit`, `tests.test_m1_boot_config`, `tests.test_a1_filter_storage` plus the new M1 case: 9 tests, one unavailable local TC1 SDK case skipped; M1/A1 actual SDK fault and power-loss boundaries exercised |
| Reviewed release-note gates | PASS | 25 tests in `tests.test_user_notes` and `tests.test_publish_user_notes`; rc.2 adds an explicit candidate chain, immutable original, 11 reviewed blocks and three bilingual fixes; historical entries are unchanged |
| Release storage preflight | PASS | Account R2 usage 1,785,957,439 bytes, below the 10 GB upload ceiling |
| Firmware CI matrix, signed assets and publication | NOT_RUN | Recorded separately after the final pinned frontend and firmware source are merged; local tests are not publication evidence |
| Reported M1 and A1 physical devices | NOT_RUN | No device write or reboot performed in this session |

The M1 integration test derives the real application size from `M1/storage.h` and
extracts production functions unchanged. A separate translation unit runs the
pinned, patched SDK against the existing faultable Flash fixture. It checks that
legacy and extended network-reset frames produce no successful receipt or writes
in recovery; a failed full clear preserves RAM and recovery; a retry commits before
ACK; duplicate committed frames do not write again; a cold boot reads the exact
accepted defaults; supported-layout network resets preserve MQTT settings; and
indeterminate commits continue to reject both reset kinds.

## Physical acceptance still required

Identify the M1 model and installed variant, record its version and recovery
state, and retain a configuration backup if the current firmware can export it.
Install the matching rc.2 image through an available OTA/recovery entry. On the
device hotspot, explicitly confirm full clearing in the page, wait for restart,
and reconnect to the hotspot. In the same browser tab, check that recovery is
gone and a new network configuration can be submitted. Reboot once more and
confirm version, connectivity and persisted configuration. Also open a fresh
page to distinguish cached UI state from continuing storage recovery.

If recovery persists after restart, record the migration state and command error
code; do not count an accepted HTTP response as a persistent clear. A1 long-press
acceptance belongs to its separate investigation.
