#!/usr/bin/env python3
"""Sampled direct-NNUE component profiler (requires `make -f Makefile.blaze profile`)."""
from __future__ import annotations

import argparse
import json
import statistics
import subprocess
import time
from pathlib import Path

from benchmark import positions, sha256


def run_one(engine: Path, nnue: Path, fen: str, threads: int) -> dict:
    process = subprocess.Popen([str(engine)], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                               text=True, encoding="utf-8", bufsize=1)
    assert process.stdin and process.stdout
    def send(line: str) -> None:
        process.stdin.write(line + "\n")
        process.stdin.flush()
    def wait(prefix: str) -> str:
        while True:
            line = process.stdout.readline().rstrip("\r\n")
            if not line:
                raise RuntimeError("engine terminated before " + prefix)
            if line.startswith(prefix):
                return line
    try:
        send("uci"); wait("uciok")
        send(f"setoption name Threads value {threads}")
        send(f"setoption name EvalFile value {nnue}")
        send("setoption name UseNNUE value true")
        send("bench_nnue_reset"); wait("info string nnue_benchmark_reset")
        send("position fen " + fen)
        started = time.monotonic_ns()
        send("go depth 8")
        info = ""
        while True:
            line = process.stdout.readline().rstrip("\r\n")
            if not line:
                raise RuntimeError("engine terminated before bestmove")
            if line.startswith("info depth "):
                info = line
            if line.startswith("bestmove "):
                best = line
                break
        elapsed = time.monotonic_ns() - started
        fields = info.split()
        def value(name: str) -> int:
            return int(fields[fields.index(name) + 1]) if name in fields else 0
        send("bench_nnue_stats")
        stats = json.loads(wait("info string nnue_benchmark ")[len("info string nnue_benchmark "):])
        stats.update({"elapsed_nanoseconds": elapsed, "bestmove": best.split()[1],
                      "nodes": value("nodes"), "qnodes": value("qnodes")})
        return stats
    finally:
        if process.poll() is None:
            send("quit")
            process.wait(timeout=5)


def aggregate(samples: list[dict]) -> dict:
    total_search_ns = sum(sample["elapsed_nanoseconds"] for sample in samples)
    component_names = samples[0]["components"].keys()
    components: dict[str, dict] = {}
    for name in component_names:
        calls = sum(sample["components"][name]["calls"] for sample in samples)
        sampled = sum(sample["components"][name]["sampled_calls"] for sample in samples)
        sampled_ns = sum(sample["components"][name]["sampled_nanoseconds"] for sample in samples)
        average_ns = sampled_ns / sampled if sampled else 0.0
        estimated_ns = average_ns * calls
        components[name] = {"calls": calls, "sampled_calls": sampled,
                            "sampled_nanoseconds": sampled_ns, "average_ns": average_ns,
                            "estimated_nanoseconds": estimated_ns,
                             "percent_search": 100.0 * estimated_ns / total_search_ns if total_search_ns else 0.0}
    delta_ns = components.get("nnue_delta", {}).get("estimated_nanoseconds", 0.0)
    for name, component in components.items():
        if name.startswith("delta_"):
            component["percent_delta"] = 100.0 * component["estimated_nanoseconds"] / delta_ns if delta_ns else 0.0
    nodes = sum(sample["nodes"] for sample in samples)
    qnodes = sum(sample["qnodes"] for sample in samples)
    inferences = sum(sample["inferences"] for sample in samples)
    full_refreshes = sum(sample["full_refreshes"] for sample in samples)
    incremental = sum(sample["incremental_evaluations"] for sample in samples)
    return {"samples": len(samples), "total_search_nanoseconds": total_search_ns,
            "median_search_ms": statistics.median(s["elapsed_nanoseconds"] for s in samples) / 1e6,
            "nodes": nodes, "qnodes": qnodes,
            "qsearch_percent": 100.0 * qnodes / nodes if nodes else 0.0,
            "evaluations_per_node": inferences / nodes if nodes else 0.0,
            "incremental_evaluation_percent": 100.0 * incremental / inferences if inferences else 0.0,
            "full_refresh_percent_of_evaluations": 100.0 * full_refreshes / inferences if inferences else 0.0,
            "components": components}


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--engine", type=Path, required=True)
    parser.add_argument("--nnue", type=Path, required=True)
    parser.add_argument("--threads", type=int, default=1)
    parser.add_argument("--runs", type=int, default=5)
    parser.add_argument("--positions", type=int, default=8)
    parser.add_argument("--seed", type=int, default=20260726)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if args.runs < 1 or args.positions < 1:
        parser.error("runs and positions must be positive")
    fens = positions(args.positions, args.seed)
    metadata = {"engine": str(args.engine.resolve()), "engine_sha256": sha256(args.engine),
                "nnue": str(args.nnue.resolve()), "threads": args.threads,
                "runs": args.runs, "positions": args.positions, "seed": args.seed,
                "sampling": "one in 64 calls"}
    records: list[dict] = []
    with args.output.open("w", encoding="utf-8") as out:
        out.write(json.dumps({"metadata": metadata}, sort_keys=True) + "\n")
        for run in range(args.runs):
            for index, fen in enumerate(fens, 1):
                sample = run_one(args.engine, args.nnue, fen, args.threads)
                sample.update({"run": run + 1, "position": index})
                records.append(sample)
                out.write(json.dumps(sample, sort_keys=True) + "\n")
        out.write(json.dumps({"aggregate": aggregate(records)}, sort_keys=True) + "\n")
    print(json.dumps(aggregate(records), indent=2, sort_keys=True))
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
