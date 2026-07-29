"""Merge Stockfish MultiPV shard annotations into a tactical corpus."""

from __future__ import annotations

import argparse
import json
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--corpus", type=Path, required=True)
    parser.add_argument("--annotations", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    corpus = json.loads(args.corpus.read_text(encoding="utf-8"))
    annotations = {}
    for path in args.annotations.rglob("top-two-*.json"):
        shard = json.loads(path.read_text(encoding="utf-8"))
        for entry in shard["positions"]:
            identifier = entry["id"]
            if identifier in annotations:
                raise ValueError(f"duplicate annotation for {identifier}")
            annotations[identifier] = entry["top_two_uci"]
    expected = {entry["id"] for entry in corpus["positions"]}
    if set(annotations) != expected:
        raise ValueError("annotations do not exactly cover the corpus")
    for entry in corpus["positions"]:
        entry["top_two_uci"] = annotations[entry["id"]]
    corpus["source"]["stockfish_multipv"] = 2
    args.output.write_text(json.dumps(corpus, indent=2) + "\n", encoding="utf-8")
    print(f"merged {len(annotations)} positions")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
