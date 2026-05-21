#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/build"
MIN_TIME="${1:-0.02s}"
FILTER="${2:-}"

cmake -S "${SCRIPT_DIR}" \
  -B "${BUILD_DIR}" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CXX_FLAGS_RELEASE="-O3 -DNDEBUG"

cmake --build "${BUILD_DIR}" --target queue_benchmark -j 4

CMD=("${BUILD_DIR}/queue_benchmark" "--benchmark_min_time=${MIN_TIME}")
if [[ -n "${FILTER}" ]]; then
  CMD+=("--benchmark_filter=${FILTER}")
fi

"${CMD[@]}"
