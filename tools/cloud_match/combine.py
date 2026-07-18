"""Combine compatible local and cloud lane evidence."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import re
from typing import Any, Iterable

from tools.experiment.match import validate_evidence_payload
from tools.experiment.pentanomial import Pentanomial, sprt_decision, sprt_llr


_COUNT_KEYS = ("wins2", "wins1_draw1", "draws2", "losses1_draw1", "losses2")
_SHA256 = re.compile(r"^[0-9a-f]{64}$")
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
    if not isinstance(common["opening_sha256"], str) or not _SHA256.fullmatch(
        common["opening_sha256"]
    ):
        raise ValueError("lanes require a frozen full-suite opening SHA-256")
    totals = [0, 0, 0, 0, 0]
    expected_games = completed_games = clean_games = clean_pairs = 0
    quarantined_games = quarantined_pairs = 0
    raw_wdl = {"wins": 0, "draws": 0, "losses": 0}
    clean_wdl = {"wins": 0, "draws": 0, "losses": 0}
    termination_counts: dict[str, dict[str, int]] | None = None
    abnormal_games: list[dict[str, Any]] = []
    lane_records: list[dict[str, Any]] = []
    opening_ranges: list[dict[str, Any]] = []
    for index, lane in enumerate(items):
        if lane.get("schema_version") != 3:
            raise ValueError(f"unsupported lane schema at index {index}")
        if _configuration(lane) != common:
            raise ValueError(f"incompatible lane configuration at index {index}")
        evidence = validate_evidence_payload(lane, context=f"lane {index}")
        values = list(evidence.counts.as_tuple())
        totals = [left + right for left, right in zip(totals, values, strict=True)]
        expected_games += evidence.expected_games
        completed_games += evidence.completed_games
        clean_games += evidence.clean_games
        clean_pairs += evidence.clean_pairs
        quarantined_games += evidence.quarantined_games
        quarantined_pairs += evidence.quarantined_pairs
        for key in raw_wdl:
            raw_wdl[key] += evidence.raw_wdl[key]
            clean_wdl[key] += evidence.clean_wdl[key]
        if termination_counts is None:
            termination_counts = {
                group: {key: 0 for key in entries}
                for group, entries in evidence.termination_counts.items()
            }
        for group, entries in evidence.termination_counts.items():
            for key, value in entries.items():
                termination_counts[group][key] += value
        lane_name = lane.get("lane", f"lane-{index}")
        lane_configuration = lane.get("configuration", lane.get("spec"))
        assert isinstance(lane_configuration, dict)
        opening_start = lane_configuration.get("opening_start")
        opening_count = lane_configuration.get("opening_count")
        if (
            not isinstance(opening_start, int)
            or isinstance(opening_start, bool)
            or opening_start <= 0
            or not isinstance(opening_count, int)
            or isinstance(opening_count, bool)
            or opening_count <= 0
            or opening_count != evidence.expected_games // 2
        ):
            raise ValueError(f"invalid opening range at lane {index}")
        opening_ranges.append(
            {
                "lane": lane_name,
                "opening_start": opening_start,
                "opening_count": opening_count,
            }
        )
        abnormal_games.extend(
            {**dict(record), "lane": lane_name}
            for record in evidence.abnormal_games
        )
        lane_records.append(
            {
                "schema_version": 3,
                "lane": lane_name,
                **evidence.to_dict(),
                "artifacts": lane.get("artifacts", {}),
            }
        )
    combined = Pentanomial(*totals)
    expected_start = 1
    for opening_range in sorted(opening_ranges, key=lambda item: item["opening_start"]):
        start = opening_range["opening_start"]
        if start < expected_start:
            raise ValueError("opening lane ranges overlap")
        if start > expected_start:
            raise ValueError("opening lane ranges leave uncovered positions")
        expected_start = start + opening_range["opening_count"]
    sprt = common["sprt"]
    if clean_pairs == 0:
        llr = 0.0
        decision = "no_clean_pairs"
    else:
        llr = sprt_llr(combined, float(sprt["elo0"]), float(sprt["elo1"]))
        decision = sprt_decision(
            combined,
            float(sprt["elo0"]),
            float(sprt["elo1"]),
            float(sprt["alpha"]),
            float(sprt["beta"]),
        )
    assert termination_counts is not None
    return {
        "schema_version": 3,
        "kind": "hybrid-meta-analysis",
        "expected_games": expected_games,
        "completed_games": completed_games,
        "clean_games": clean_games,
        "clean_pairs": clean_pairs,
        "quarantined_games": quarantined_games,
        "quarantined_pairs": quarantined_pairs,
        "raw_wdl": raw_wdl,
        "clean_wdl": clean_wdl,
        "counts": dict(zip(_COUNT_KEYS, combined.as_tuple(), strict=True)),
        "termination_counts": termination_counts,
        "abnormal_games": abnormal_games,
        "llr": llr,
        "decision": decision,
        "configuration": common,
        "opening_ranges": opening_ranges,
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
