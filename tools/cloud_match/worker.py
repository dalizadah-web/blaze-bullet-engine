"""Execute one deterministic shard of a cloud match."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import platform
from typing import Any

from tools.cloud_match.shards import pair_indexes
from tools.cloud_match.spec import CloudMatchSpec
from tools.experiment.manifest import ArtifactIdentity, sha256_file
from tools.experiment.match import MatchEvidence, MatchSpec, SprtSpec, run_match


def _globalize_evidence(
    evidence: MatchEvidence, game_ids: list[str]
) -> dict[str, Any]:
    if len(game_ids) != evidence.expected_games:
        raise ValueError("game ID count does not match evidence")
    payload = {"schema_version": 3, **evidence.to_dict()}
    records: list[dict[str, object]] = []
    for raw_record in evidence.abnormal_games:
        record = dict(raw_record)
        game_index = record.pop("game_index", None)
        if (
            not isinstance(game_index, int)
            or isinstance(game_index, bool)
            or not 0 <= game_index < len(game_ids)
        ):
            raise ValueError("abnormal game index is outside shard assignment")
        record["game_id"] = game_ids[game_index]
        records.append(record)
    payload["abnormal_games"] = records
    return payload


def write_shard_openings(
    source: Path | str,
    indexes: list[int],
    destination: Path | str,
    *,
    opening_start: int = 1,
) -> list[str]:
    lines = [
        line.strip()
        for line in Path(source).read_text(encoding="utf-8-sig").splitlines()
        if line.strip()
    ]
    if not lines:
        raise ValueError(f"opening source contains no opening positions: {source}")
    if opening_start <= 0:
        raise ValueError("opening_start must be a positive one-based index")
    source_indexes = [opening_start - 1 + index for index in indexes]
    if len(set(source_indexes)) != len(source_indexes):
        raise ValueError("duplicate opening source index")
    if any(index < 0 or index >= len(lines) for index in source_indexes):
        raise ValueError("opening source index is outside opening source; wraparound is forbidden")
    selected = [lines[index] for index in source_indexes]
    target = Path(destination)
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text("".join(line + "\n" for line in selected), encoding="utf-8")
    return selected


def _resolve_openings(spec_path: Path, openings: str) -> Path:
    supplied = Path(openings)
    candidates = [supplied] if supplied.is_absolute() else [spec_path.parent / supplied, Path.cwd() / supplied]
    for candidate in candidates:
        if candidate.is_file():
            return candidate.resolve()
    raise ValueError(f"opening file does not exist: {openings}")


def run_worker(
    *,
    spec_path: Path | str,
    shard_index: int,
    candidate: Path | str,
    baseline: Path | str,
    runner: Path | str,
    output: Path | str,
    source_commit: str,
) -> dict[str, Any]:
    config_path = Path(spec_path).resolve()
    spec = CloudMatchSpec.from_json(config_path)
    spec.validate_frozen()
    assigned_pairs = pair_indexes(spec.games, shard_index, spec.shards)
    if not assigned_pairs:
        raise ValueError(f"shard {shard_index} has no assigned pairs")

    output_path = Path(output).resolve()
    if output_path.exists() and any(output_path.iterdir()):
        raise ValueError(f"shard output directory is not empty: {output_path}")
    output_path.mkdir(parents=True, exist_ok=True)

    source_openings = _resolve_openings(config_path, spec.openings)
    if sha256_file(source_openings) != spec.opening_sha256:
        raise ValueError("source opening SHA-256 mismatch")
    shard_openings = output_path / "shard-openings.epd"
    write_shard_openings(
        source_openings,
        assigned_pairs,
        shard_openings,
        opening_start=spec.opening_start,
    )

    candidate_identity = ArtifactIdentity.from_path(candidate)
    baseline_identity = ArtifactIdentity.from_path(baseline)
    runner_identity = ArtifactIdentity.from_path(runner)
    if candidate_identity.sha256 != spec.candidate_sha256:
        raise ValueError("candidate binary SHA-256 does not match frozen spec")
    if baseline_identity.sha256 != spec.baseline_sha256:
        raise ValueError("baseline binary SHA-256 does not match frozen spec")
    match_spec = MatchSpec(
        schema_version=2,
        name=f"{spec.name}-shard-{shard_index:02d}",
        games=len(assigned_pairs) * 2,
        concurrency=spec.concurrency,
        time_control=spec.time_control,
        threads=spec.threads,
        hash_mb=spec.hash_mb,
        repeat=True,
        opening_format="epd",
        openings=str(shard_openings),
        opening_sha256=sha256_file(shard_openings),
        opponent_sha256=baseline_identity.sha256,
        sprt=SprtSpec(
            elo0=spec.sprt.elo0,
            elo1=spec.sprt.elo1,
            alpha=spec.sprt.alpha,
            beta=spec.sprt.beta,
        ),
        opening_start=1,
    )
    match_output = output_path / "match"
    result = run_match(
        match_spec,
        candidate=candidate_identity.path,
        opponent=baseline_identity.path,
        runner=runner_identity.path,
        output=match_output,
        source_commit=source_commit,
        candidate_name="Candidate",
        opponent_name="Baseline",
    )

    experiment_id = spec.experiment_id()
    game_ids = [
        game_id
        for pair in assigned_pairs
        for game_id in (
            f"{experiment_id}-p{pair:06d}-w",
            f"{experiment_id}-p{pair:06d}-b",
        )
    ]
    payload: dict[str, Any] = {
        **_globalize_evidence(result.evidence, game_ids),
        "experiment_id": experiment_id,
        "shard_index": shard_index,
        "shard_count": spec.shards,
        "candidate_commit": spec.candidate_commit,
        "baseline_commit": spec.baseline_commit,
        "candidate_sha256": candidate_identity.sha256,
        "baseline_sha256": baseline_identity.sha256,
        "openings_sha256": spec.opening_sha256,
        "runner_sha256": runner_identity.sha256,
        "pair_indexes": assigned_pairs,
        "source_opening_indexes": [spec.opening_start + pair for pair in assigned_pairs],
        "game_ids": game_ids,
        "pgn": "match/games.pgn",
        "environment": {
            "machine": platform.machine(),
            "os": platform.platform(),
            "processor": platform.processor(),
        },
    }
    (output_path / "shard.json").write_text(
        json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    return payload


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--spec", type=Path, required=True)
    parser.add_argument("--shard-index", type=int, required=True)
    parser.add_argument("--candidate", type=Path, required=True)
    parser.add_argument("--baseline", type=Path, required=True)
    parser.add_argument("--runner", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--source-commit", required=True)
    args = parser.parse_args()
    result = run_worker(
        spec_path=args.spec,
        shard_index=args.shard_index,
        candidate=args.candidate,
        baseline=args.baseline,
        runner=args.runner,
        output=args.output,
        source_commit=args.source_commit,
    )
    print(json.dumps(result, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
