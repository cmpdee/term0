#!/usr/bin/env bash
set -euo pipefail

root_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
build_dir="${root_dir}/build"

cmake -S "$root_dir" -B "$build_dir" -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build "$build_dir" --parallel

echo
echo "Built: ${build_dir}/term0"
echo "Run with: ${build_dir}/term0"
