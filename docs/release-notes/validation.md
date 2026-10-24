# Static release-note validation

Recorded on 2026-10-24, against the 23 published archives through v0.9.23.

- PASS: 23 pure comparison scenarios (numeric versions, lifecycle, replacement,
  exact interval merging, independent fixes, scopes, source identities and errors).
- PASS: 15 client/jsdom scenarios, including resource integrity, one-shot info,
  authentication/busy/legacy/timeout/permission errors and stale reply isolation.
- PASS: 13 Python migration/provenance/translation/CLI scenarios.
- PASS: 21 existing public documentation checks and retained archive unit checks.
- PASS: formal local site build against Pages commit
  `b9a64b055064322ef86d04ad392d711716fa9914`; actual signed firmware, source commits
  and historical frontend locks verified. All 340 retained archive/OTA files are
  byte-identical. The fixed snapshot hash is recorded in archive-identities.json.
- PASS: Chromium desktop 1280px, mobile 390px, Chinese/English, same-version and
  pending-review outcomes, no-JavaScript history, and homepage (7 scenarios).
  Screenshots/report: ignored `build/notes-evidence/`.
- PASS: this static change does not touch A1/M1/TC1/common firmware sources,
  frontend.lock.json, package-lock.json or firmware packaging inputs. The optional
  v0.9.24 device entry and its subsequent frontend lock are separate changes.
- NOT RUN in this record: real old-device automatic reading and production
  publication. Browser simulations cannot establish CORS or local-network browser
  permissions on a real deployment. See the final integrated release record for
  later physical-device, firmware-package and publication results.

TC1's reported power-loss reconnect incident was identified by its owner as a
configuration operation error. It is not a firmware defect or release blocker.
The independent blank-MQTT-host/port-zero configuration API fix is tracked in the
firmware changes separately.
