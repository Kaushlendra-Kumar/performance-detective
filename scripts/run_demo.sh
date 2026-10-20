#!/usr/bin/env bash
# Builds Performance Detective, starts the dashboard and profiles every example workload
# through the HTTP API so the dashboard has real sessions to explore.
set -euo pipefail
cd "$(dirname "$0")/.."
PORT=${PORT:-7878}
cmake --preset default >/dev/null
cmake --build --preset default -j"$(nproc)"
./build/detective demo-data --out frontend/demo/demo-session.json >/dev/null
./build/detective serve --port "$PORT" --data-dir "${DATA_DIR:-detective-data}" &
SERVER=$!
trap 'kill $SERVER 2>/dev/null' EXIT
sleep 1
python3 scripts/profile_examples.py --base "http://127.0.0.1:$PORT" --examples "$PWD/build/examples"
echo
echo "Dashboard: http://127.0.0.1:$PORT   (Ctrl-C to stop)"
wait $SERVER
