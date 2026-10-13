#!/usr/bin/env bash
set -euo pipefail
project_dir="$(cd "$(dirname "$0")" && pwd)"
# Build sequentially so explicitly shared tool caches are safe on first use.
for product in TC1 M1 A1; do
    echo "Building $product"
    "$project_dir/$product/build.sh"
done
