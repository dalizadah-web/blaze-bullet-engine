"""Combine compatible local and cloud lane evidence."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
from typing import Any, Iterable

from tools.experiment.pentanomial import Pentanomial, sprt_decision, sprt_llr


_COUNT_KEYS = ("wins2", "wins1_draw1", "draws2", "losses1_draw1", "losses2")
_COMPATIBILITY_KEYS = (
    "candidate_ref",
    "baseline_ref",
    "time_control",
    "threads",
    "hash_mb",
    "opening_sha256",
    "sprt",
)


def _configuration(lane: dict[str, Any]) -> dict[str, Any]:
    source = lane.get("configuration", lane.get("spec"))
    if not isinstance(source, dict):
        raise ValueError("lane has no configuration")
    return {key: source.get(key) for key in _COMPATIBILITY_KEYS}


def combine_lanes(lanes: Iterable[dict[str, Any]]) -> dict[str, Any]:
    items = list(lanes)
    if len(items) < 2:
        raise ValueError("at least two lanes are required")
    common = _configuration(items[0])
    totals = [0, 0, 0, 0, 0]
    lane_records = []
    for index, lane in enumerate(items):
        if _configuration(lane) != common:
            raise ValueError(f"incompatible lane configuration at index {index}")
        raw_counts = lane.get("counts")
        if not isinstance(raw_counts, dict) or set(raw_counts) != set(_COUNT_KEYS):
            raise ValueError(f"invalid lane counts at index {index}")
        values = [raw_counts[key] for key in _COUNT_KEYS]
        if any(not isinstance(value, int) or isinstance(value, bool) or value < 0 for value in values):
            raise ValueError(f"invalid lane count value at index {index}")
        counts = Pentanomial(*values)
        if lane.get("games") != counts.pairs * 2:
            raise ValueError(f"lane game count mismatch at index {index}")
        totals = [left + right for left, right in zip(totals, values, strict=True)]
        lane_records.append(
            {
                "lane": lane.get("lane", f"lane-{index}"),
                "games": lane["games"],
                "counts": raw_counts,
                "artifacts": lane.get("artifacts", {}),
            }
        )
    combined = Pentanomial(*totals)
    sprt = common["sprt"]
    llr = sprt_llr(combined, float(sprt["elo0"]), float(sprt["elo1"]))
    decision = sprt_decision(
        combined,
        float(sprt["elo0"]),
        float(sprt["elo1"]),
        float(sprt["alpha"]),
        float(sprt["beta"]),
    )
    return {
        "schema_version": 1,
        "kind": "hybrid-meta-analysis",
        "games": combined.pairs * 2,
        "pairs": combined.pairs,
        "counts": dict(zip(_COUNT_KEYS, combined.as_tuple(), strict=True)),
        "llr": llr,
        "decision": decision,
        "configuration": common,
        "lanes": lane_records,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--lanes", type=Path, nargs="+", required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    lanes = [json.loads(path.read_text(encoding="utf-8")) for path in args.lanes]
    result = combine_lanes(lanes)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(json.dumps(result, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
