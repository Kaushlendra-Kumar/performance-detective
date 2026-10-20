#!/usr/bin/env bash
# Builds and runs every test configuration: default, ASan+UBSan, TSan.
set -euo pipefail
cd "$(dirname "$0")/.."
jobs=$(nproc)
for preset in default asan tsan; do
  echo "=== $preset ==="
  cmake --preset "$preset" >/dev/null
  cmake --build --preset "$preset" -j"$jobs"
  if [ "$preset" = tsan ]; then
    # Concurrency-relevant suites; profiling-heavy tests are slow under TSan.
    (cd build-tsan && ./unit_tests && ./integration_tests "[api],[process],[session]")
  else
    ctest --preset "$preset"
  fi
done
echo "All configurations passed."
