#!/usr/bin/env bash
# CI must fail rather than silently skip ARM regressions after tool path changes.
set -euo pipefail
: "${ARM_CC:?Set ARM_CC to the pinned compiler}"
test -x "$ARM_CC"
test "$("$ARM_CC" -dumpversion)" = 5.4.1
test "$("$ARM_CC" -dumpmachine)" = arm-none-eabi
command -v qemu-arm
qemu-arm --version
output=$(mktemp -d)
trap 'rm -rf "$output"' EXIT
for abi in soft hard; do
    printf 'int arm_ci_probe(void) { return 0; }\n' | "$ARM_CC" -mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi="$abi" -x c -c - -o "$output/$abi.o"
done
