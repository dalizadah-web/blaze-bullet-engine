"""Annotate one deterministic corpus shard with Stockfish MultiPV moves."""

from __future__ import annotations

import argparse
import json
from pathlib import Path

import chess
import chess.engine


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--corpus", type=Path, required=True)
    parser.add_argument("--stockfish", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--depth", type=int, required=True)
    parser.add_argument("--shard-index", type=int, required=True)
    parser.add_argument("--shard-count", type=int, required=True)
    args = parser.parse_args()
    if not 0 <= args.shard_index < args.shard_count:
        parser.error("shard-index must be within shard-count")

    corpus = json.loads(args.corpus.read_text(encoding="utf-8"))
    positions = corpus["positions"]
    selected = positions[args.shard_index :: args.shard_count]
    engine_path = args.stockfish.resolve()
    if not engine_path.is_file():
        parser.error(f"Stockfish binary does not exist: {engine_path}")

    engine = chess.engine.SimpleEngine.popen_uci(str(engine_path))
    engine.configure({"Threads": 1, "Hash": 16})
    annotations = []
    try:
        for position in selected:
            board = chess.Board(position["fen"])
            infos = engine.analyse(
                board, chess.engine.Limit(depth=args.depth), multipv=2
            )
            annotations.append(
                {
                    "id": position["id"],
                    "top_two_uci": [info["pv"][0].uci() for info in infos],
                }
            )
    finally:
        engine.quit()

    payload = {
        "depth": args.depth,
        "shard_index": args.shard_index,
        "shard_count": args.shard_count,
        "positions": annotations,
    }
    args.output.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")
    print(f"annotated {len(annotations)} positions")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
