OTA signing uses P-256 and SHA-256. Install `cryptography==46.0.3` and invoke:

```
python3 tools/ota/package.py --key /path/outside/repository/release-key.pem \
  --model tc1 --version v0.9.2 --commit FULL_40_CHARACTER_GIT_SHA \
  phicomm_tc1_a1.ota.bin.raw phicomm_tc1_a1.ota.bin.signed
```

Models: `tc1`, `m1`, `a1`; key_id defaults to 1. The key must match
`common/ota/keyring.h`. CI only signs tested `v*` tags on main ancestry.
The private key is never part of any build artifact. Preserve original raw
`.ota.bin.raw` and `.all.bin` files for legacy bootstrapping and recovery.

Legacy firmware cannot interpret the signed envelope. First install the new
verifier firmware using its native `.ota.bin.raw` through the existing trusted
local upload procedure. Subsequent default uploads and all URL/MQTT downloads
must use `.ota.bin.signed`. Never upload the signed envelope to a raw bootloader.

Run `tests/ota/run.sh`; use `TC1_TEST_IMAGE` to point the TC1 adapter tests at a
built native OTA image. Tests generate their own temporary keys. Hardware timing,
stack/heap and power interruption tests remain separate acceptance activities.
