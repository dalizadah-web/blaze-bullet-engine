"""Measure exact best-move agreement on the Stockfish first-swing corpus."""

from __future__ import annotations

import argparse
import json
from pathlib import Path

from tools.search_signature import UciEngine


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--engine", type=Path, required=True)
    parser.add_argument("--corpus", type=Path, required=True)
    parser.add_argument("--nodes", type=int, default=50_000)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    corpus = json.loads(args.corpus.read_text(encoding="utf-8"))["positions"]
    results = []
    engine = UciEngine(str(args.engine.resolve()), threads=1, hash_mb=16, timeout=30.0)
    try:
        for index, entry in enumerate(corpus, start=1):
            result = engine.search(entry["fen"], args.nodes)
            hit = result["bestmove"] == entry["best_uci"]
            results.append(
                {
                    "id": entry["id"],
                    "motif": entry["motif"],
                    "expected": entry["best_uci"],
                    "actual": result["bestmove"],
                    "hit": hit,
                }
            )
            if index % 25 == 0 or index == len(corpus):
                print(f"benchmarked {index}/{len(corpus)} positions")
    finally:
        engine.close()

    by_motif = {}
    for motif in sorted({entry["motif"] for entry in results}):
        subset = [entry for entry in results if entry["motif"] == motif]
        by_motif[motif] = {
            "positions": len(subset),
            "hits": sum(entry["hit"] for entry in subset),
        }
    payload = {
        "nodes": args.nodes,
        "positions": len(results),
        "hits": sum(entry["hit"] for entry in results),
        "by_motif": by_motif,
        "mistakes": [entry for entry in results if not entry["hit"]],
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"hits": payload["hits"], "positions": payload["positions"]}))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
