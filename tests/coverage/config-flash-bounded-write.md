# Bounded SDK configuration writes

The pinned MOC kernel allocates a temporary buffer equal to each Flash write's
length. Its application API wrapper discards the underlying error status. A
single application configuration write can therefore fail on a fragmented heap
while returning success without advancing the Flash offset.

`common/config-write-bounded-mico36.patch` limits system, application and extra-AP
writes to 1024 bytes per call. It checks offset progress after each call. The
existing full-image verification, CRC commit, backup recovery and indeterminate
commit protections remain in effect. The final CRC write retains its existing
read-back handling for an ambiguous programming result.

Run `python3 -m unittest -v tests.test_config_commit` with `A1_SDK_DIR`,
`M1_SDK_DIR` and `TC1_SDK_DIR` pointing to the pinned SDK checkouts. Tests operate
on disposable copies and include:

- Actual SDK commit and cold-boot functions with a 4648-byte application image,
  both with and without extra AP storage.
- A 1024-byte simulated kernel write-allocation limit, plus a silent failure
  without offset progress at every write boundary.
- The previous actual SDK overlay as a negative control: it must fail the same
  allocation-limit scenario before the new overlay is applied.
- The existing I/O, partial-write, allocation, power-loss, backup and uncertain
  commit matrix, with address/undefined-behaviour sanitizers.
- Ordered overlay preparation from pristine and previously patched caches,
  including existing product overlays, idempotence and incompatible-cache
  rejection without modification.

These tests reproduce the allocation mechanism. A device's `kWriteErr` alone
does not identify which underlying Flash operation failed.
