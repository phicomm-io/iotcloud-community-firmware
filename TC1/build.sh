#!/usr/bin/env bash
set -euo pipefail
app_dir="$(cd "$(dirname "$0")" && pwd)"
project_dir="$(dirname "$app_dir")"
variant="${PHICOMM_FIRMWARE_VARIANT:-full}"
case "$variant" in
    full) variant_suffix=""; PHICOMM_FULL_RESOURCES=1 ;;
    no-frontend-resources) variant_suffix="-no-frontend-resources"; PHICOMM_FULL_RESOURCES=0 ;;
    *) echo "Unknown firmware variant: $variant" >&2; exit 1 ;;
esac
export PHICOMM_FULL_RESOURCES
export PHICOMM_WEB_OUTPUT_DIR="$project_dir/build/tc1${variant_suffix}-web"

sdk_dir="${TC1_SDK_DIR:-$project_dir/build/tc1-sdk-3.6.4/mico-os}"
workspace="$project_dir/build/tc1${variant_suffix}-workspace-3.6.4"
python3 "$project_dir/tools/prepare_sdk.py" "$sdk_dir"
python3 "$project_dir/tools/apply_sdk_patch.py" "$sdk_dir" "$project_dir/common/easylink-bonjour-bounds.patch"
python3 "$project_dir/tools/apply_sdk_patch.py" "$sdk_dir" "$project_dir/common/bonjour-lan-mdns-oom.patch" "$project_dir/common/bonjour-lan-mdns-compact.patch"
# Validate configuration recovery and commit overlays as one ordered stack.
python3 "$project_dir/tools/apply_sdk_patch.py" "$sdk_dir" \
    "$project_dir/common/config-recovery-mico36.patch" \
    "$project_dir/common/config-commit-mico36.patch" \
    "$project_dir/common/config-write-bounded-mico36.patch"
python3 "$project_dir/tools/apply_sdk_patch.py" "$sdk_dir" "$app_dir/mqtt-handlers-sdk.patch"
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
python3 "$project_dir/tools/apply_sdk_patch.py" "$sdk_dir" "$project_dir/common/wifi-owned-mico36.patch"
# Validate the overlapping MQTT overlays together, including repeated builds.
python3 "$project_dir/tools/apply_sdk_patch.py" "$sdk_dir" \
    "$project_dir/common/mqtt-mico36.patch" \
    "$project_dir/common/mqtt-publish-bounds.patch" \
    "$project_dir/common/mqtt-socket-close.patch" \
    "$project_dir/M1/mqtt-packet-deadline.patch" \
    "$project_dir/common/mqtt-response-deadline.patch" \
    "$project_dir/common/mqtt-buffer-storage.patch" \
    "$project_dir/common/mqtt-subscribe-qos.patch" \
    "$project_dir/common/mqtt-subscribe-result.patch"
python3 "$project_dir/tools/apply_sdk_patch.py" "$sdk_dir" "$app_dir/http-retained-sdk.patch"
python3 "$project_dir/tools/apply_sdk_patch.py" "$sdk_dir" "$app_dir/parameters-sdk.patch"
python3 "$project_dir/tools/apply_sdk_patch.py" "$sdk_dir" "$app_dir/signed-ota-sdk.patch"
python3 "$project_dir/tools/apply_sdk_patch.py" "$sdk_dir" "$app_dir/no-unsigned-sdk-write.patch"
python3 "$project_dir/tools/install_tc1_native_sdk.py" "$sdk_dir"
mkdir -p "$workspace"
ln -sfn "$sdk_dir" "$workspace/mico-os"
ln -sfn "$app_dir" "$workspace/TC1"
ln -sfn "$project_dir/common/httpd" "$workspace/phicomm_httpd"
ln -sfn "$project_dir/common/mqtt" "$workspace/phicomm_mqtt"
tools_dir="${TC1_TOOLS_DIR:-$project_dir/build/tc1-tools}"
target=TC1@MK3031@moc
version_args=()
if [[ "${GITHUB_REF_TYPE:-}" == tag ]]; then
    version_args=(--tag "$GITHUB_REF_NAME")
fi
python3 "$project_dir/tools/check_version.py" "${version_args[@]}"
mkdir -p "$tools_dir"
gcc_dir="$tools_dir/gcc-arm-none-eabi-5_4-2016q2"
python2="${TC1_PYTHON2:-$tools_dir/python2/bin/python2.7}"
if [[ ! -x "$gcc_dir/bin/arm-none-eabi-gcc" ]]; then
    curl -fsSL --retry 3 'https://launchpad.net/gcc-arm-embedded/5.0/5-2016-q2-update/+download/gcc-arm-none-eabi-5_4-2016q2-20160622-linux.tar.bz2' -o "$tools_dir/gcc.tar.bz2"
    echo "9910b6b5df12efe564dbb3856bf1599d4c16178a6f28bd8a23c9e5c3edc219e4  $tools_dir/gcc.tar.bz2" | sha256sum -c -
    tar -xjf "$tools_dir/gcc.tar.bz2" -C "$tools_dir"
fi
if [[ ! -x "$python2" ]]; then
    curl -fsSL --retry 3 https://www.python.org/ftp/python/2.7.18/Python-2.7.18.tgz -o "$tools_dir/python.tgz"
    echo "da3080e3b488f648a3d7a4560ddee895284c3380b11d6de75edb986526b9a814  $tools_dir/python.tgz" | sha256sum -c -
    tar -xzf "$tools_dir/python.tgz" -C "$tools_dir"
    (
        cd "$tools_dir/Python-2.7.18"
        ./configure --prefix="$tools_dir/python2" > "$tools_dir/python-build.log" 2>&1
        make -j4 >> "$tools_dir/python-build.log" 2>&1
        make install >> "$tools_dir/python-build.log" 2>&1
    )
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
    cd "$project_dir/TC1/http_server"
    python3 ./test.py
)
(
    cd "$workspace"
    make HOST_OS=Linux64 TOOLS_ROOT="$micoder" PYTHON="$python2" TLS=mocSSL \
        -f mico-os/makefiles/Makefile "$target" total
)
python3 "$project_dir/tools/check_http_core.py" "$workspace/build/$target/Modules" TC1 "$gcc_dir/bin/arm-none-eabi-nm" "$workspace/build/$target/binary/$target.elf"
python3 "$project_dir/tools/check_lan_stack.py" "$workspace/build/$target/Modules" TC1 "$gcc_dir/bin/arm-none-eabi-nm" "$workspace/build/$target/binary/$target.elf"
python3 "$project_dir/tools/check_no_json_objects.py" --model tc1 --nm "$gcc_dir/bin/arm-none-eabi-nm" "$workspace/build/$target/binary/$target.elf"
python3 "$project_dir/tools/check_tc1_trace.py" "$gcc_dir/bin/arm-none-eabi-nm" "$workspace/build/$target/binary/$target.elf"
python3 "$app_dir/tools/package.py" "$workspace/build/$target/binary" "$project_dir/build/tc1${variant_suffix}-release"
