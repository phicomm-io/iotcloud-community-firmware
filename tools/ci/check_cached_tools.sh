#!/usr/bin/env bash
# Validate only restored tool installations; product scripts rebuild missing ones.
set -euo pipefail
tools="$PWD/build/ci-tools"
gcc="$tools/gcc-arm-none-eabi-5_4-2016q2"
python="$tools/python2"
if [[ -d "$gcc" ]] && ! [[ "$("$gcc/bin/arm-none-eabi-gcc" -dumpversion 2>/dev/null || true)" == 5.4.1 ]]; then
    echo 'Discarding incompatible GCC cache'
    rm -rf "$gcc"
fi
if [[ -d "$python" ]] && ! "$python/bin/python2.7" -c 'import os,sys,json,distutils.sysconfig; assert sys.version_info[:3] == (2,7,18); assert os.path.realpath(sys.prefix) == os.path.realpath(sys.argv[1]); assert os.path.realpath(distutils.sysconfig.get_config_var("prefix")) == os.path.realpath(sys.argv[1])' "$python"; then
    echo 'Discarding incompatible or relocated Python cache'
    rm -rf "$python" "$tools/Python-2.7.18"
fi
