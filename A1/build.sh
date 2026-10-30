#!/usr/bin/env bash
set -euo pipefail
app_dir="$(cd "$(dirname "$0")" && pwd)"
project_dir="$(dirname "$app_dir")"
version_args=()
if [[ "${GITHUB_REF_TYPE:-}" == tag ]]; then
    version_args=(--tag "$GITHUB_REF_NAME")
fi
python3 "$project_dir/tools/check_version.py" "${version_args[@]}"
variant="${PHICOMM_FIRMWARE_VARIANT:-full}"
A1_FRESH_AIR=0
case "$variant" in
    full) variant_suffix=""; PHICOMM_FULL_RESOURCES=1 ;;
    no-frontend-resources) variant_suffix="-no-frontend-resources"; PHICOMM_FULL_RESOURCES=0 ;;
    fresh-air) variant_suffix="-fresh-air"; PHICOMM_FULL_RESOURCES=1; A1_FRESH_AIR=1 ;;
    fresh-air-no-frontend-resources) variant_suffix="-fresh-air-no-frontend-resources"; PHICOMM_FULL_RESOURCES=0; A1_FRESH_AIR=1 ;;
    *) echo "Unknown firmware variant: $variant" >&2; exit 1 ;;
esac
export PHICOMM_FULL_RESOURCES A1_FRESH_AIR
export PHICOMM_WEB_OUTPUT_DIR="$project_dir/build/a1${variant_suffix}-web"
build_type="${PHICOMM_A1_BUILD_TYPE:-release}"
case "$build_type" in
    release|release_log) ;;
    *) echo "Unsupported A1 SDK build type: $build_type" >&2; exit 1 ;;
esac
export PHICOMM_A1_BUILD_TYPE="$build_type"
echo "A1 SDK build type: $build_type"

sdk_dir="${A1_SDK_DIR:-$project_dir/build/a1-sdk/mico-os}"
shared_tools="${PHICOMM_TOOLS_HOME:-${XDG_CACHE_HOME:-$HOME/.cache}/phicomm}"
compiler_major="${PHICOMM_BUILD_COMPILER_EXPECTED:-5.4.1}"
compiler_major="${compiler_major%%.*}"
tools_dir="${A1_TOOLS_DIR:-$shared_tools/profiles/gcc$compiler_major}"
workspace="$project_dir/build/a1${variant_suffix}-workspace"
sdk_commit="$(python3 "$project_dir/tools/prepare_sdk.py" --commit)"
target=A1@MK3080B@moc
if [[ "$build_type" == release ]]; then target+=@release; fi
export PHICOMM_A1_BUILD_TARGET="$target"
mkdir -p "$tools_dir" "$workspace" "$(dirname "$sdk_dir")"

python3 "$project_dir/tools/prepare_sdk.py" "$sdk_dir"
# Binary numbers and the allocation-free HA decimal writer do not use printf float.
python3 "$project_dir/tools/install_nano_printf_sdk.py" "$sdk_dir"
python3 "$project_dir/tools/apply_sdk_patch.py" "$sdk_dir" "$project_dir/common/easylink-bonjour-bounds.patch"
python3 "$project_dir/tools/apply_sdk_patch.py" "$sdk_dir" "$project_dir/common/bonjour-lan-mdns-oom.patch" "$project_dir/common/bonjour-lan-mdns-compact.patch"
# Validate configuration recovery and commit overlays as one ordered stack.
python3 "$project_dir/tools/apply_sdk_patch.py" "$sdk_dir" \
    "$project_dir/common/config-recovery-mico36.patch" \
    "$project_dir/common/config-commit-mico36.patch" \
    "$project_dir/common/config-write-bounded-mico36.patch"
# Validate overlapping board overlays as one idempotent patch stack.
python3 "$project_dir/tools/apply_sdk_patch.py" "$sdk_dir" \
    "$app_dir/sdk.patch" "$app_dir/features-sdk.patch" "$app_dir/fresh-air-sdk.patch"

python3 "$project_dir/tools/apply_sdk_patch.py" "$sdk_dir" "$project_dir/common/watchdog-mico36.patch"
# Validate the overlapping MQTT overlays together, including repeated builds.
python3 "$project_dir/tools/apply_sdk_patch.py" "$sdk_dir" \
    "$project_dir/common/mqtt-mico36.patch" \
    "$project_dir/common/mqtt-publish-bounds.patch" \
    "$project_dir/common/mqtt-socket-close.patch" \
    "$project_dir/M1/mqtt-packet-deadline.patch" \
    "$project_dir/common/mqtt-response-deadline.patch" \
    "$project_dir/common/mqtt-buffer-storage.patch" \
    "$project_dir/common/mqtt-subscribe-qos.patch" \
    "$project_dir/common/mqtt-subscribe-result.patch" \
    "$project_dir/common/mqtt-receive-recovery.patch" \
    "$project_dir/common/mqtt-post-send-workspace.patch"
# Isolate newlib floating conversion scratch across HTTP/MQTT/runtime threads.
# Validate these overlapping overlays together, including on repeated builds.
python3 "$project_dir/tools/apply_sdk_patch.py" "$sdk_dir" \
    "$project_dir/common/json-newlib-reent.patch" \
    "$project_dir/common/json-printbuf-oom.patch" \
    "$project_dir/common/json-allocation-safety.patch" \
    "$project_dir/common/json-heap-budget.patch" \
    "$project_dir/common/json-newlib-nano-abi.patch" \
    "$project_dir/common/json-integer-parse.patch" \
    "$project_dir/common/json-sized-resize.patch" \
    "$project_dir/common/json-number-cache-cleanup.patch" \
    "$project_dir/common/json-number-checked.patch"
python3 "$project_dir/tools/install_json_numeric.py" "$sdk_dir"

ln -sfn "$sdk_dir" "$workspace/mico-os"
ln -sfn "$app_dir" "$workspace/A1"
ln -sfn "$project_dir/common/httpd" "$workspace/phicomm_httpd"
ln -sfn "$project_dir/common/mqtt" "$workspace/phicomm_mqtt"
python3 "$app_dir/tools/pack_web.py"

gcc_dir="$tools_dir/gcc-arm-none-eabi-5_4-2016q2"
python2="${A1_PYTHON2:-$tools_dir/python2/bin/python2.7}"
if [[ ! -x "$gcc_dir/bin/arm-none-eabi-gcc" || ! -x "$python2" ]]; then
    if [[ "$tools_dir" != "$shared_tools/profiles/gcc$compiler_major" ]]; then
        echo "Custom tool profile is incomplete: $tools_dir; use tools/prepare_toolchain.py in the shared cache" >&2
        exit 1
    fi
    python3 "$project_dir/tools/prepare_toolchain.py" --home "$shared_tools" \
        --profile "gcc$compiler_major" --log-dir "$project_dir/build/toolchain-logs"
fi
compiler_version="$($gcc_dir/bin/arm-none-eabi-gcc -dumpversion)"
expected_compiler_version="${PHICOMM_BUILD_COMPILER_EXPECTED:-5.4.1}"
case "$expected_compiler_version" in
    5.4.1|14.3.1) ;;
    *) echo "Unsupported ARM GCC expectation: $expected_compiler_version" >&2; exit 1 ;;
esac
[[ "$compiler_version" == "$expected_compiler_version" ]] || {
    echo "Expected ARM GCC $expected_compiler_version, got $compiler_version" >&2
    exit 1
}

micoder="$tools_dir/MiCoder"
mkdir -p "$micoder/cmd/Linux64" "$micoder/compiler/arm-none-eabi-5_4-2016q2-20160622"
ln -sfn "$gcc_dir" "$micoder/compiler/arm-none-eabi-5_4-2016q2-20160622/Linux64"
for tool in cat cp echo mkdir mv rm dash; do
    ln -sfn "$(type -P "$tool")" "$micoder/cmd/Linux64/$tool"
done
ln -sfn "$(command -v make)" "$micoder/cmd/Linux64/make"
(
    cd "$workspace"
    make HOST_OS=Linux64 TOOLS_ROOT="$micoder" PYTHON="$python2" TLS=mocSSL \
        DISABLE_MICO_BLUENRG=1 -f mico-os/makefiles/Makefile "$target" total
)
python3 "$app_dir/tools/check_button_stack.py" "$workspace/build/$target/Modules/A1" "$app_dir/a1.h"
python3 "$project_dir/tools/check_worker_stack.py" "$workspace/build/$target/Modules/A1" "$project_dir/common/phicomm_features.h" "$gcc_dir/bin/arm-none-eabi-nm"
python3 "$app_dir/tools/check_image.py" "$workspace/build/$target/binary/$target.ota.bin"
python3 "$project_dir/tools/check_http_core.py" "$workspace/build/$target/Modules" A1 "$gcc_dir/bin/arm-none-eabi-nm" "$workspace/build/$target/binary/$target.elf"
python3 "$project_dir/tools/check_lan_stack.py" "$workspace/build/$target/Modules" A1 "$gcc_dir/bin/arm-none-eabi-nm" "$workspace/build/$target/binary/$target.elf"
python3 "$project_dir/tools/check_no_json_objects.py" --model a1 --nm "$gcc_dir/bin/arm-none-eabi-nm" "$workspace/build/$target/binary/$target.elf"
python3 "$app_dir/tools/package.py" "$workspace/build/$target/binary" "$project_dir/build/a1${variant_suffix}-release" "$sdk_commit"
