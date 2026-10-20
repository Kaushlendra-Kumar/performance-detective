#!/usr/bin/env python3
"""Profiles every example workload through the Performance Detective HTTP API, waits for
completion, prints a summary and runs the before/after comparisons. Standard library only."""
import argparse
import json
import time
import urllib.error
import urllib.request


def call(base, method, path, body=None):
    data = json.dumps(body).encode() if body is not None else None
    headers = {"Content-Type": "application/json"} if body is not None else {}
    req = urllib.request.Request(base + path, method=method, data=data, headers=headers)
    try:
        with urllib.request.urlopen(req) as r:
            return r.status, json.loads(r.read())
    except urllib.error.HTTPError as e:
        return e.code, json.loads(e.read())


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--base", default="http://127.0.0.1:7878")
    ap.add_argument("--examples", required=True, help="directory with the built example programs")
    a = ap.parse_args()
    E = a.examples
    jobs = [
        dict(label="A slow (baseline)", executable=E + "/inefficient_loop", args=["--mode", "slow"], benchmark_runs=5, warmup_runs=1),
        dict(label="A fast (candidate)", executable=E + "/inefficient_loop", args=["--mode", "fast"], benchmark_runs=5, warmup_runs=1),
        dict(label="B allocations naive", executable=E + "/allocation_stress", args=["--mode", "naive"], track_allocations=True),
        dict(label="B allocations reuse", executable=E + "/allocation_stress", args=["--mode", "reuse"], track_allocations=True),
        dict(label="C recursion naive", executable=E + "/recursive_workload", args=["--mode", "naive"]),
        dict(label="D contention coarse", executable=E + "/contention_demo", args=["--mode", "coarse"], benchmark_runs=5),
        dict(label="D contention fine", executable=E + "/contention_demo", args=["--mode", "fine"], benchmark_runs=5),
        dict(label="E data naive", executable=E + "/data_processing", args=["--impl", "naive"], benchmark_runs=5),
        dict(label="E data fast", executable=E + "/data_processing", args=["--impl", "fast"], benchmark_runs=5),
        dict(label="Instrumented pipeline", executable=E + "/instrumented_demo", args=["--requests", "40000"], mode="instrumentation"),
        dict(label="Stripped binary", executable=E + "/inefficient_loop_stripped", args=[]),
    ]
    ids = {}
    for j in jobs:
        st, s = call(a.base, "POST", "/api/sessions?start=1", j)
        if st != 201:
            raise SystemExit(f"could not create {j['label']}: {s}")
        ids[j["label"]] = s["id"]
    print(f"queued {len(ids)} sessions; waiting…")
    while True:
        states = {k: call(a.base, "GET", "/api/sessions/" + v)[1]["state"] for k, v in ids.items()}
        if all(s in ("COMPLETED", "FAILED", "CANCELLED") for s in states.values()):
            break
        time.sleep(1)
    for k, v in ids.items():
        s = call(a.base, "GET", "/api/sessions/" + v)[1]
        sm = s.get("summary", {})
        tf = s.get("top_findings", [])
        print(f"{s['state']:9} {k:24} wall={sm.get('wall_seconds', 0):.3f}s "
              f"samples={sm.get('sampling', {}).get('total_samples', '-')} top={tf[0]['title'][:60] if tf else '-'}")
    for x, y in [("A slow (baseline)", "A fast (candidate)"), ("D contention coarse", "D contention fine"),
                 ("E data naive", "E data fast")]:
        _, c = call(a.base, "GET", f"/api/compare?baseline={ids[x]}&candidate={ids[y]}")
        m = c["metrics"][0]
        print(f"compare {x} -> {y}: {m['relative_change'] * 100:+.1f}% wall, verdict={m['verdict']}")


if __name__ == "__main__":
    main()
