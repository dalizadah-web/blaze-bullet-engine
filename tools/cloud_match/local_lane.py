"""Run a high-concurrency local lane compatible with cloud evidence."""

from __future__ import annotations

import argparse
from dataclasses import asdict, replace
import json
from pathlib import Path

from tools.cloud_match.spec import CloudMatchSpec
from tools.experiment.manifest import ArtifactIdentity, sha256_file
from tools.experiment.match import MatchSpec, SprtSpec, run_match


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--base-spec", type=Path, required=True)
    parser.add_argument("--candidate-ref", required=True)
    parser.add_argument("--baseline-ref", required=True)
    parser.add_argument("--candidate", type=Path, required=True)
    parser.add_argument("--baseline", type=Path, required=True)
    parser.add_argument("--runner", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--games", type=int, required=True)
    parser.add_argument("--concurrency", type=int, default=8)
    parser.add_argument("--time-control", required=True)
    parser.add_argument("--source-commit", required=True)
    args = parser.parse_args()

    base = CloudMatchSpec.from_json(args.base_spec)
    openings = Path(base.openings).resolve(strict=True)
    if sha256_file(openings) != base.opening_sha256:
        raise ValueError("opening SHA-256 mismatch")
    baseline_identity = ArtifactIdentity.from_path(args.baseline)
    spec = MatchSpec(
        schema_version=1,
        name=f"{base.name}-local",
        games=args.games,
        concurrency=args.concurrency,
        time_control=args.time_control,
        threads=base.threads,
        hash_mb=base.hash_mb,
        repeat=True,
        opening_format="epd",
        openings=str(openings),
        opening_sha256=base.opening_sha256,
        opponent_sha256=baseline_identity.sha256,
        sprt=SprtSpec(**asdict(base.sprt)),
    )
    result = run_match(
        spec,
        candidate=args.candidate,
        opponent=baseline_identity.path,
        runner=args.runner,
        output=args.output,
        source_commit=args.source_commit,
        candidate_name="Candidate",
        opponent_name="Baseline",
    )
    configuration = asdict(
        replace(
            base,
            candidate_ref=args.candidate_ref,
            baseline_ref=args.baseline_ref,
            games=args.games,
            shards=1,
            time_control=args.time_control,
        )
    )
    summary = {
        "schema_version": 1,
        "lane": "local-windows-16-thread-cpu",
        "games": result.games,
        "counts": asdict(result.counts),
        "llr": result.llr,
        "decision": result.decision,
        "configuration": configuration,
        "local_concurrency": args.concurrency,
        "artifacts": {
            "candidate_sha256": ArtifactIdentity.from_path(args.candidate).sha256,
            "baseline_sha256": baseline_identity.sha256,
            "runner_sha256": ArtifactIdentity.from_path(args.runner).sha256,
            "openings_sha256": base.opening_sha256,
        },
    }
    (args.output / "lane-summary.json").write_text(
        json.dumps(summary, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    print(json.dumps(summary, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
