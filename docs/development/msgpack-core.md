# MessagePack core

Compile `common/typed_wire.c` and `common/wire_buffer.c` once per product. The
core needs C99/GCC, standard allocation and string routines, and IEEE binary32 /
binary64. It has no SDK, JSON object, JSON numeric, printf, printbuf or CBOR
implementation dependency. Default product activation belongs to transport and
business integration; adding this core alone does not migrate any route.

`ph_wire_format` contains only `PH_WIRE_MSGPACK=1`; other format arguments fail.
The format argument remains in the API to reduce business migration changes.

## Input view and profile

`phv_parse(format, bytes, length, &root)` validates exactly one complete
MessagePack document. Check success before examining root or derived views.
All views and iterators borrow the immutable input, never allocate, and expire
when its owner releases or reuses it. Synchronous handlers can borrow. Before
queueing asynchronous work, decode and copy validated business fields into an
independently owned structure. Copying a `ph_value` does not copy its payload.
`phv_text` decodes a string into a caller-owned C string; `phv_bytes` returns a
borrowed byte pointer plus explicit length, so byte fields can contain NUL.

Limits are 8192 input bytes, 1024 total nodes including map keys, and 12 nested
containers. String map keys must be shorter than 128 bytes. Strings require
valid UTF-8 and prohibit embedded NUL to match the C-string API; arbitrary
binary uses MessagePack bin8/bin16/bin32 (`PHV_BYTES`). Empty binary is valid.
Duplicate string keys, nonstring keys, invalid UTF-8, trailing bytes, extension
values and nonfinite floats are rejected. Integers stay within ±(2^53−1),
matching the browser's exact integer range. Finite float32/float64 input is
supported; output uses binary64 and does not perform decimal conversion.

Duplicate detection has a bounded comparison budget, so adversarial collision
maps may be rejected rather than consuming unbounded work. Iterators and lookup
revisit validated input; no constant-time lookup is claimed. `phv_keys` uses the
existing `|key||key|` whitelist grammar and rejects empty or pipe-containing
input keys to prevent delimiter aliases. This is a structural parser, not
business validation: handlers must still enforce allowed keys, types, bounds
and authorization before mutating state.

## Output and memory ownership

`ph_buf` has `char *buf; size_t len, cap;`. `len` is the binary payload length;
`cap` includes an extra trailing sentinel byte that is not part of the payload.
Never use strlen to send it. An owned writer transfers its buffer through
`phw_take`; retire it with `ph_buf_free`. Failed take frees partial output.
`phw_discard` also retires partial owned output. Do not copy a live writer.
Fixed and measure modes borrow the caller's `ph_buf` and optional byte storage;
finish with `phw_finish_fixed`, where zero means failure. Do not free borrowed
storage with `ph_buf_free`. Sizing must replay pure output, not execute business
mutations again. A `phw_root_key` must survive until the following value call.

The direct writer allows 16384 output bytes and 12 nested containers. These are
independent from the smaller input limit: large responses are not necessarily
valid command payloads. Backup/config messages larger than 8192 input bytes need
an explicit chunked protocol or a separately reviewed limit change; this core
does not silently enlarge device memory budgets. Output names must meet the
same string-key size rule. Writers are trusted schema emitters: they do not
keep a duplicate-key table or enforce the input node budget. Business schemas
must emit unique names. Same-format container copies pass through the normal
iterative writer so subtree embedding cannot bypass depth limits.
`phw_value_canonical` re-encodes scalar values but preserves map iteration order;
it is not sorted-map canonicalization and must not be used as a semantic replay
hash without an explicit higher-level ordering contract.

`phw_cached_value(writer, key, cached, depth)` is an internal fast path for a
self-generated, previously validated single encoded value. The cache owner must
provide its true maximum container depth (zero for scalars), preserve immutable
storage during the call and never alias writer storage. It checks the remaining
writer depth and output budget, emits one member prefix, then copies the bytes
without parsing or serializing the subtree. Never pass unvalidated network or
request data to this trusted-cache interface. It provides no validation of the
encoded value or of the caller-supplied depth metadata.

The default weak `ph_wire_malloc` / `ph_wire_free` hooks use malloc/free. A
product may override both for its allocator and admission budget. They must use
the same allocator and be safe for concurrent callers. Allocation growth copies
only the live length plus sentinel. Failed growth leaves the original buffer
intact. Parsing, fixed writing and measurement do not allocate.

## Verification boundary

Run `python3 -m unittest tests.test_msgpack_core`, optionally with `SANITIZE=1`
for ASan/UBSan. These host fixtures compile the production C translation units
without SDK stubs. Golden protocol bytes, borrowed binary fields, NUL handling,
UTF-8/duplicate/size/count/depth/truncation rejection, bounded malformed fuzz,
safe integer and exact binary64 roundtrips, OOM cleanup, transactional growth,
fixed-buffer canaries and subtree embedding are covered. Host results do not
prove device stack size, performance, local-button priority or OTA behavior.
Those require the product build gates and live-device scenarios separately.
