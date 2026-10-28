#!/usr/bin/env bash
# Historical pinned SDK JSON library coverage. No product/service/runtime files.
set -euo pipefail
cd "$(dirname "$0")/.."
json_source=${PHICOMM_TEST_JSON_DIR:-build/a1-sdk/mico-os/libraries/utilities/json_c}
if [[ ! -f "$json_source/json.h" ]]; then
  echo 'Set PHICOMM_TEST_JSON_DIR to the pinned historical MiCO json_c directory' >&2
  exit 1
fi
output=$(mktemp -d)
trap 'rm -rf "$output"' EXIT
python3 - "$json_source" "$output/sdk" <<'PY'
from tests.sdk_json import copy_patched_json
from pathlib import Path
import sys
json = copy_patched_json(Path(sys.argv[1]), Path(sys.argv[2]))
for path in json.glob('*.c'):
    path.write_text(path.read_text().replace('%ld','%d'))
PY
json_dir="$output/sdk/libraries/utilities/json_c"
PHICOMM_TEST_JSON_DIR="$json_dir" python3 -m unittest \
  tests.test_json_integer_parse.JsonIntegerParseTest.test_integer_conversion_ignores_errno_host_sanitizers \
  tests.test_json_sized_resize tests.test_json_numeric_link \
  tests.test_json_float32.JsonFloat32.test_host_roundtrip_and_capacity
compiler=("${CC:-cc}")
if [[ "${SANITIZE:-0}" == 1 ]]; then
  compiler+=(-g -fno-omit-frame-pointer -fsanitize=address,undefined -no-pie)
fi
flags=(-std=gnu99 -Wall -Wextra -Werror -Wno-misleading-indentation -Wno-unused-parameter -Wno-sign-compare -Wno-implicit-fallthrough -Wno-unused-but-set-variable -Wno-unused-value -Wno-nonnull-compare -Itests/stubs -I"$json_dir")
"${compiler[@]}" "${flags[@]}" tests/test_json_printbuf_failure.c "$json_dir"/*.c -Wl,--wrap=json_heap_resize -o "$output/printbuf"
"$output/printbuf"
"${compiler[@]}" "${flags[@]}" tests/test_json_allocation_failure.c "$json_dir"/*.c -Wl,--wrap=malloc -Wl,--wrap=calloc -Wl,--wrap=realloc -Wl,--wrap=strdup -o "$output/allocation"
"$output/allocation"
"${compiler[@]}" "${flags[@]}" -pthread tests/test_json_heap.c "$json_dir"/*.c -Wl,--wrap=malloc -Wl,--wrap=calloc -Wl,--wrap=realloc -Wl,--wrap=strdup -Wl,--wrap=free -o "$output/heap"
"$output/heap"
# Private newlib cache/stack history requires the optional pinned ARM/QEMU lane.
# It remains available explicitly, rather than silently skipped by host CI.
if [[ "${LEGACY_SDK_ARM:-0}" == 1 ]]; then
  PHICOMM_TEST_JSON_DIR="$json_dir" python3 -m unittest \
    tests.test_json_number_cache tests.test_json_numeric_stack tests.test_json_numeric_checked \
    tests.test_json_integer_parse.JsonIntegerParseTest.test_integer_conversion_ignores_errno_arm_nano \
    tests.test_json_float32.JsonFloat32.test_pinned_arm_roundtrip_and_allocation_failures
fi
