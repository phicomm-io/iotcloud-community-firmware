# S30-V1B configuration migration startup recovery

The RC5 build at `c09a067` completed OTA on S30-V1B but rebooted repeatedly.
UART1 at 115200 8N1 recorded `of--app` after the saved Wi-Fi configuration was
loaded. The pinned BK7231N SDK emits this message from `rtos_stack_overflow()`;
the `app` task has a 4096-byte stack.

The importer and initial business settings occupy the same startup frame.
Migration then calls persistence, which previously added a complete disk record
to that stack. The same persistence routine also runs from the 2048-byte
QuickTick task.

| Native GCC 5.4.1 static frame | Before | After |
| --- | ---: | ---: |
| `PH_BL_Init` (including importer) | 2440 B | 2440 B |
| Persistence implementation | 1280 B | 128 B |
| `PH_BL_Tick` | 32 B | 32 B |

The comparison recompiles both source revisions with the same native flags and
SDK headers. These figures exclude callees and interrupt context. The native
build now retains compiler stack reports and checks frame budgets before
discarding its temporary build tree.

Persistence allocates its 1152-byte record temporarily and releases it after
the synchronous write. An allocation failure returns a storage failure without
opening the temporary file. The committed record remains intact. The existing
write/sync/close/rename sequence and PBL1/PBL2/PBL3 import layouts are unchanged.

ASan/UBSan regression covers allocation failure and all five I/O failure stages
for legacy import and current records. Failed migrations preserve the original
bytes and successfully retry; all record buffers are released. The complete
S30 business and shared HTTP/MQTT/UDP suite passed. The native build and stock
UART/RBL package checks passed for the initial fix `594444e8`.

Hardware recovery is waiting for the module RX connection: the user reported a
detached RX pad. The Mac receives startup logs on UART channel 91, while cold
restart and all four UART probes receive no BootROM acknowledgement. No flash
writes were made during these recovery attempts. The c09a067 S30 download is
withdrawn from both Pages catalogs and its GitHub release carries a warning.

Next hardware checks: read two matching full-flash backups, verify factory MAC,
write only the application at `0x11000`, compare complete flash readback, boot
and verify settings migration/Wi-Fi/MQTT/HTTP. Restore the original boot-on
policy after the OTA guard is removed. Reissue the final S30 download after
these checks pass.
