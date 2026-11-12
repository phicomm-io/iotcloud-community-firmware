# MX nano printf link selection, 2026-10-28

A1 and M1 keep newlib-nano and the SDK/application formatters, but no longer
force the optional floating-point printf implementation into the application.
TC1 already uses this linker selection; its build profile is unchanged.
This does not remove binary floating-point MessagePack values or the exact HA
decimal writer.

## Caller audit

The audit used the retained input sections in the actual GCC 5 maps from
`cc9ca4afc6e2467df782a01d0cf77b681239c680`, rather than the cross-reference table,
which also names discarded sections. It inspected the associated archive ELF
relocations to printf, sprintf, snprintf, their `v` and reentrant variants, and
format strings in live application/SDK sections. Compressed web resources are
opaque byte arrays, not format strings.

| Build | Retained app/SDK objects | Sections calling formatters | Format constants | Executable float formats found |
| --- | ---: | ---: | ---: | ---: |
| A1 fresh-air GCC 5 | 64 | 58 | 131 | 0 |
| M1 GCC 5 | 65 | 46 | 104 | 0 |
| TC1 GCC 5 | 78 | 86 | 224 | 0 |

All retained SDK formatter inputs use integer/string conversions, including
provisioning, configuration storage, network notifications, mDNS, MAC address
formatting, and logging. `custom_log` concatenates its format argument with a
literal prefix. The retained `user_handler` callback table exports no application
printf callback to the ROM library. This audit does not change the SDK sources,
logging calls, ROM library, console support, or allocation policy.

The application's generic `mqtt_format` callers also supply literal or
conditional-literal formats. A1's two apparent `% e` matches are `{% endif %}`
inside HA Jinja templates, passed through `ha_json_string`, not printf. The
floating-point snprintf calls in `common/json_numeric/format.c` are the host
fallback under `#ifndef __NEWLIB__`, not these target callers.

MessagePack numbers use the binary writer. A1's HA decimal values use
`ph_decimal_format` and `ph_decimal_format_float32`, an allocation-free integer
implementation. M1's HA decimal values use scaled integer fields and integer
printf conversions. JSON string escaping, UTF-8 validation, parser checks and
all memory guards remain in place.

## Link and regression evidence

An isolated archive substitution/relink preserved every other original object
and option. Recompiling the unmodified status source first reproduced the
original allocated object section sizes. Removing only `-u _printf_float`
saved **6,440 application bytes** in both A1 and M1 GCC 5; it did not change BSS.
Combined with model-specific diagnostics pruning, the trials still exceed the
262,144-byte application budget:

| GCC 5 trial | Used bytes | Overflow bytes |
| --- | ---: | ---: |
| A1 pruning + link selection | 264,516 | 2,372 |
| M1 pruning + link selection | 266,132 | 3,988 |

These are failed-link capacity trials, not installable firmware. Further
capacity work and the complete post-link stack/ABI/configuration/resource gates
are required. The originally labelled A1/M1 GCC 14 matrix cells contained GCC 5
objects; they are not evidence of a clean GCC 14 build.

`tests.test_nano_printf_sdk` and `tests.test_tc1_native_sdk` pass (4 tests),
including byte-preserving CRLF installation, idempotence, build hook ordering,
and rejection of unknown/duplicate/mixed linker layouts without modifying the
input. `tests.test_numeric_feed` and `tests.test_ha_codec_shared` pass (11 tests,
ASan/UBSan), covering the actual binary numeric producer and shared HA
writer/parser/discovery code, including the expected failing negative control.
The A1/M1/TC1 shell build scripts pass `bash -n`.

The target link trials and caller audit are static/link evidence. No new DUT
write, physical acceptance, or ARM-native execution was performed for this
change. Future floating-point printf callers must explicitly revisit this
linker profile or use the existing bounded decimal writer.
