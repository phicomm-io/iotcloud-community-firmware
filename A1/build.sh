#!/usr/bin/env bash
set -euo pipefail
app_dir="$(cd "$(dirname "$0")" && pwd)"
project_dir="$(dirname "$app_dir")"
python3 "$project_dir/tools/check_version.py"
sdk_dir="${A1_SDK_DIR:-$project_dir/build/a1-sdk/mico-os}"
tools_dir="${A1_TOOLS_DIR:-$project_dir/build/a1-tools}"
workspace="$project_dir/build/a1-workspace"
sdk_commit=6c465211d3ff8797cd835e400ec54a06530dd476
target=A1@MK3080B@moc
mkdir -p "$tools_dir" "$workspace" "$(dirname "$sdk_dir")"

if [[ ! -d "$sdk_dir/.git" ]]; then
    git clone --depth 1 --branch 3.6.4 https://github.com/MXCHIP/mico-os.git "$sdk_dir"
fi
[[ "$(git -C "$sdk_dir" rev-parse HEAD)" == "$sdk_commit" ]] || { echo 'A1 SDK commit mismatch' >&2; exit 1; }
if git -C "$sdk_dir" apply --reverse --check "$app_dir/sdk.patch" 2>/dev/null; then
    : # already applied
else
    git -C "$sdk_dir" apply --check "$app_dir/sdk.patch"
    git -C "$sdk_dir" apply "$app_dir/sdk.patch"
fi

# Root TC1 SDK/version/build.sh are intentionally not part of this workspace.
if ! git -C "$sdk_dir" apply --reverse --check "$app_dir/features-sdk.patch" 2>/dev/null; then
    git -C "$sdk_dir" apply --check "$app_dir/features-sdk.patch"
    git -C "$sdk_dir" apply "$app_dir/features-sdk.patch"
fi

ln -sfn "$sdk_dir" "$workspace/mico-os"
ln -sfn "$app_dir" "$workspace/A1"
python3 "$app_dir/tools/pack_web.py"

gcc_dir="$tools_dir/gcc-arm-none-eabi-5_4-2016q2"
python2="${A1_PYTHON2:-$tools_dir/python2/bin/python2.7}"
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
[[ "$compiler_version" == 5.4.1 ]] || { echo 'Expected ARM GCC 5.4.1' >&2; exit 1; }

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
python3 "$app_dir/tools/check_image.py" "$workspace/build/$target/binary/$target.ota.bin"
python3 "$app_dir/tools/package.py" "$workspace/build/$target/binary" "$project_dir/build/a1-release" "$sdk_commit"
