The `publisher.msgpack` fixture was generated with the actual
`tools/release/device_manifest.py` `document()` and `encoded()` functions from
the rc4 release-metadata worktree at
`414c9cb8666ddef7908b01256fd8dde9ea784221`. `publisher.json` records the same document.
The version, commit and filenames are a representative contract fixture; size
and digest are synthetic. This is not an uploaded release or DUT evidence.

The reader tests compile the actual `common/ota/release_manifest.h` and shared
MessagePack implementation. The independent test encoder deliberately emits
nonminimal integer/string representations as well as the compact publisher
fixture. Input is overwritten before the parsed structure is examined.

Set `NATIVE_MP_ROOT` to test another complete firmware source checkout; by
default tests compile their own checkout. Both this module and
`tests.test_http_product_mp` support `SANITIZE=1` for ASan/UBSan.
