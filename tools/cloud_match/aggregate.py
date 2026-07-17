"""Validate and aggregate immutable cloud match shards."""

from __future__ import annotations

import argparse
from dataclasses import asdict
import json
from pathlib import Path
import re
from typing import Any, Iterable

from tools.cloud_match.shards import pair_indexes
from tools.cloud_match.spec import CloudMatchSpec
from tools.experiment.pentanomial import Pentanomial, sprt_decision, sprt_llr


_SHA256 = re.compile(r"^[0-9a-f]{64}$")
_GIT_SHA1 = re.compile(r"^[0-9a-f]{40}$")
_COUNT_KEYS = ("wins2", "wins1_draw1", "draws2", "losses1_draw1", "losses2")

# Commit identifiers are 40-char Git SHA-1; binary/opening/runner identities
# are 64-char SHA-256. Map each required shard key to its digest regex.
_COMMIT_KEYS = ("candidate_commit", "baseline_commit")
_HASH_KEYS = ("candidate_sha256", "baseline_sha256", "openings_sha256", "runner_sha256")


def _load_manifest(path: Path) -> dict[str, Any]:
    try:
        payload = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise ValueError(f"cannot read shard manifest {path}: {error}") from error
    if not isinstance(payload, dict):
        raise ValueError(f"shard manifest must be an object: {path}")
    return payload


def _validated_counts(raw: Any, path: Path) -> Pentanomial:
    if not isinstance(raw, dict) or set(raw) != set(_COUNT_KEYS):
        raise ValueError(f"invalid pentanomial counts in {path}")
    values = []
    for key in _COUNT_KEYS:
        value = raw[key]
        if not isinstance(value, int) or isinstance(value, bool) or value < 0:
            raise ValueError(f"invalid {key} count in {path}")
        values.append(value)
    return Pentanomial(*values)


def aggregate_shards(
    manifest_paths: Iterable[Path | str], spec: CloudMatchSpec
) -> dict[str, Any]:
    spec.validate_frozen()
    paths = [Path(path).resolve() for path in manifest_paths]
    if len(paths) != spec.shards:
        raise ValueError(
            f"expected {spec.shards} shard manifests, found {len(paths)}"
        )

    experiment_id = spec.experiment_id()
    expected_indexes = set(range(spec.shards))
    seen_indexes: set[int] = set()
    seen_game_ids: set[str] = set()
    common_hashes: dict[str, str] = {}
    totals = [0, 0, 0, 0, 0]

    for path in paths:
        raw = _load_manifest(path)
        if raw.get("schema_version") != 1:
            raise ValueError(f"unsupported shard schema in {path}")
        if raw.get("experiment_id") != experiment_id:
            raise ValueError(f"experiment ID mismatch in {path}")
        if raw.get("shard_count") != spec.shards:
            raise ValueError(f"shard count mismatch in {path}")
        index = raw.get("shard_index")
        if not isinstance(index, int) or isinstance(index, bool) or index not in expected_indexes:
            raise ValueError(f"invalid shard index in {path}")
        if index in seen_indexes:
            raise ValueError(f"duplicate shard index {index}")
        seen_indexes.add(index)

        assigned_pairs = pair_indexes(spec.games, index, spec.shards)
        if raw.get("pair_indexes") != assigned_pairs:
            raise ValueError(f"pair assignment mismatch in {path}")
        expected_games = len(assigned_pairs) * 2
        if raw.get("expected_games") != expected_games:
            raise ValueError(f"game count mismatch in {path}")

        expected_ids = [
            game_id
            for pair in assigned_pairs
            for game_id in (
                f"{experiment_id}-p{pair:06d}-w",
                f"{experiment_id}-p{pair:06d}-b",
            )
        ]
        game_ids = raw.get("game_ids")
        if game_ids != expected_ids:
            if isinstance(game_ids, list) and seen_game_ids.intersection(game_ids):
                raise ValueError(f"duplicate game ID in {path}")
            raise ValueError(f"game ID assignment mismatch in {path}")
        duplicates = seen_game_ids.intersection(game_ids)
        if duplicates:
            raise ValueError(f"duplicate game ID in {path}: {min(duplicates)}")
        seen_game_ids.update(game_ids)

        # Frozen identity checks: every shard must carry resolved commits and
        # binary hashes, and they must be consistent across all shards.
        for key in _COMMIT_KEYS:
            value = raw.get(key)
            if not isinstance(value, str) or not _GIT_SHA1.fullmatch(value):
                raise ValueError(f"invalid {key} in {path}")
            if value != getattr(spec, key):
                raise ValueError(f"{key} does not match frozen spec in {path}")
            previous = common_hashes.setdefault(key, value)
            if previous != value:
                raise ValueError(f"inconsistent {key} across shards")
        for key in _HASH_KEYS:
            value = raw.get(key)
            if not isinstance(value, str) or not _SHA256.fullmatch(value):
                raise ValueError(f"invalid {key} in {path}")
            if key in ("candidate_sha256", "baseline_sha256") and value != getattr(spec, key):
                raise ValueError(f"{key} does not match frozen spec in {path}")
            if key == "openings_sha256" and value != spec.opening_sha256:
                raise ValueError(f"opening artifact mismatch in {path}")
            previous = common_hashes.setdefault(key, value)
            if previous != value:
                raise ValueError(f"inconsistent {key} across shards")

        pgn = raw.get("pgn")
        if not isinstance(pgn, str) or not (path.parent / pgn).is_file():
            raise ValueError(f"missing shard PGN for {path}")
        counts = _validated_counts(raw.get("counts"), path)
        if counts.pairs != len(assigned_pairs):
            raise ValueError(f"pentanomial pair count mismatch in {path}")
        totals = [left + right for left, right in zip(totals, counts.as_tuple(), strict=True)]

    if seen_indexes != expected_indexes:
        raise ValueError("missing shard indexes")
    if len(seen_game_ids) != spec.games:
        raise ValueError("incomplete or duplicate game ID coverage")

    counts = Pentanomial(*totals)
    llr = sprt_llr(counts, spec.sprt.elo0, spec.sprt.elo1)
    decision = sprt_decision(
        counts,
        spec.sprt.elo0,
        spec.sprt.elo1,
        spec.sprt.alpha,
        spec.sprt.beta,
    )
    return {
        "schema_version": 1,
        "experiment_id": experiment_id,
        "games": spec.games,
        "pairs": counts.pairs,
        "shards": spec.shards,
        "counts": dict(zip(_COUNT_KEYS, counts.as_tuple(), strict=True)),
        "llr": llr,
        "decision": decision,
        "artifacts": common_hashes,
        "spec": asdict(spec),
    }


def summary_markdown(result: dict[str, Any]) -> str:
    counts = result["counts"]
    return (
        f"# Cloud match {result['experiment_id']}\n\n"
        f"- Games: {result['games']} ({result['pairs']} complete color-swapped pairs)\n"
        f"- Shards: {result['shards']}\n"
        f"- SPRT decision: **{result['decision']}**\n"
        f"- LLR: `{result['llr']:.6f}`\n"
        f"- Pentanomial: `{counts['wins2']} {counts['wins1_draw1']} "
        f"{counts['draws2']} {counts['losses1_draw1']} {counts['losses2']}`\n"
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--spec", type=Path, required=True)
    parser.add_argument("--shards", type=Path, nargs="+", required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    result = aggregate_shards(args.shards, CloudMatchSpec.from_json(args.spec))
    args.output.mkdir(parents=True, exist_ok=True)
    (args.output / "summary.json").write_text(
        json.dumps(result, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    markdown = summary_markdown(result)
    (args.output / "summary.md").write_text(markdown, encoding="utf-8")
    print(markdown, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
