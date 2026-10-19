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
python3 "$project_dir/tools/apply_sdk_patch.py" "$sdk_dir" \
    "$project_dir/common/json-newlib-reent.patch" \
    "$project_dir/common/json-printbuf-oom.patch" \
    "$project_dir/common/json-allocation-safety.patch" \
    "$project_dir/common/json-heap-budget.patch" \
    "$project_dir/common/json-newlib-nano-abi.patch"
python3 "$project_dir/tools/apply_sdk_patch.py" "$sdk_dir" "$project_dir/common/wifi-owned-mico36.patch"
python3 "$project_dir/tools/apply_sdk_patch.py" "$sdk_dir" "$project_dir/common/mqtt-mico36.patch"
python3 "$project_dir/tools/apply_sdk_patch.py" "$sdk_dir" "$project_dir/common/mqtt-publish-bounds.patch"
python3 "$project_dir/tools/apply_sdk_patch.py" "$sdk_dir" "$project_dir/common/mqtt-socket-close.patch"
python3 "$project_dir/tools/apply_sdk_patch.py" "$sdk_dir" "$project_dir/M1/mqtt-packet-deadline.patch"
python3 "$project_dir/tools/apply_sdk_patch.py" "$sdk_dir" "$project_dir/common/mqtt-response-deadline.patch"
python3 "$project_dir/tools/apply_sdk_patch.py" "$sdk_dir" \
    "$app_dir/http-connection-sdk.patch" "$app_dir/http-resource-sdk.patch" \
    "$app_dir/http-client-limit-sdk.patch" "$app_dir/http-auth-sdk.patch" \
    "$app_dir/http-cors-sdk.patch" "$app_dir/http-cors-empty-sdk.patch" "$app_dir/http-options-sdk.patch" "$app_dir/http-client-once-sdk.patch"
python3 "$project_dir/tools/apply_sdk_patch.py" "$sdk_dir" "$app_dir/parameters-sdk.patch"
python3 "$project_dir/tools/apply_sdk_patch.py" "$sdk_dir" "$app_dir/signed-ota-sdk.patch"
python3 "$project_dir/tools/apply_sdk_patch.py" "$sdk_dir" "$app_dir/no-unsigned-sdk-write.patch"
mkdir -p "$workspace"
ln -sfn "$sdk_dir" "$workspace/mico-os"
ln -sfn "$app_dir" "$workspace/TC1"
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
bash "$project_dir/tools/build_ota_tls.sh" TC1 "$gcc_dir/bin/arm-none-eabi-"
compiler_version="$($gcc_dir/bin/arm-none-eabi-gcc -dumpversion)"
[[ "$compiler_version" == 5.4.1 ]] || { echo 'Expected ARM GCC 5.4.1' >&2; exit 1; }

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
python3 "$project_dir/tools/check_json_float.py" "$gcc_dir/bin/arm-none-eabi-nm" "$workspace/build/$target/binary/$target.elf"
python3 "$app_dir/tools/package.py" "$workspace/build/$target/binary" "$project_dir/build/tc1${variant_suffix}-release"
