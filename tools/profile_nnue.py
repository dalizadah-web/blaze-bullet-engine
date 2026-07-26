#!/usr/bin/env python3
"""Reproducible depth/node NNUE profiling matrix.

This is intentionally a harness, not an acceptance-result file: it emits the
host, individual samples, medians, and IQR so results are never copied between
machines or compiler configurations.  Run it after `make -f Makefile.blaze
blaze`; production defaults remain classical.
"""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path

from benchmark import compiler_identity, cpu_name, positions, run_one, sha256, summarize_rates


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--engine", type=Path, required=True)
    parser.add_argument("--nnue", type=Path, required=True)
    parser.add_argument("--positions", type=int, default=8)
    parser.add_argument("--runs", type=int, default=5)
    parser.add_argument("--seed", type=int, default=20260726)
    parser.add_argument("--depths", type=int, nargs="*", default=[4, 6, 8],
                        help="fixed depths; pass --depths with no values for node-only runs")
    parser.add_argument("--nodes", type=int, nargs="*", default=[])
    parser.add_argument("--threads", type=int, nargs="+", default=[1, 2, 4, 8])
    parser.add_argument("--milliseconds", type=int, default=1000)
    parser.add_argument("--backend", choices=("classical", "scalar", "avx2"), default="avx2")
    args = parser.parse_args()
    if args.runs < 5:
        parser.error("--runs must be at least five so median and variance are meaningful")
    if args.positions < 1 or any(value < 1 for value in args.threads):
        parser.error("positions and thread counts must be positive")
    if not args.depths and not args.nodes:
        parser.error("supply at least one depth or fixed-node budget")

    engine = args.engine.resolve()
    fens = positions(args.positions, args.seed)
    old_force_scalar = os.environ.get("BLAZE_NNUE_FORCE_SCALAR")
    if args.backend == "scalar":
        os.environ["BLAZE_NNUE_FORCE_SCALAR"] = "1"
    else:
        os.environ.pop("BLAZE_NNUE_FORCE_SCALAR", None)
    try:
        print("profile_metadata=" + json.dumps({
            "engine": str(engine), "engine_sha256": sha256(engine),
            "nnue": str(args.nnue.resolve()), "cpu": cpu_name(),
            "compiler": compiler_identity(), "backend": args.backend,
            "runs": args.runs, "positions": args.positions, "seed": args.seed,
        }, sort_keys=True))
        cases = [(depth, 0) for depth in args.depths] + [(0, nodes) for nodes in args.nodes]
        for threads in args.threads:
            for depth, nodes in cases:
                samples: list[float] = []
                for run in range(args.runs):
                    for index, fen in enumerate(fens):
                        selected_nnue = None if args.backend == "classical" else args.nnue
                        completed_depth, count, best, elapsed = run_one(
                            engine, fen, args.milliseconds, threads, depth, nodes, selected_nnue)
                        rate = count * 1000.0 / max(elapsed, 1)
                        samples.append(rate)
                        print(json.dumps({"run": run + 1, "position": index + 1,
                                          "threads": threads, "depth": completed_depth,
                                          "node_budget": nodes, "nodes": count, "elapsed_ms": elapsed,
                                          "nps": rate, "bestmove": best}, sort_keys=True))
                summary = summarize_rates(samples)
                summary.update({"threads": threads, "requested_depth": depth,
                                "node_budget": nodes, "backend": args.backend,
                                "samples": len(samples)})
                print("profile_summary=" + json.dumps(summary, sort_keys=True))
    finally:
        if old_force_scalar is None:
            os.environ.pop("BLAZE_NNUE_FORCE_SCALAR", None)
        else:
            os.environ["BLAZE_NNUE_FORCE_SCALAR"] = old_force_scalar
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
