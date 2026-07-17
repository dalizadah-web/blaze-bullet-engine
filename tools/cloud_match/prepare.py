"""Create a validated immutable match spec from workflow inputs.

Two-stage freeze:
  1. prepare_spec resolves the mutable candidate/baseline refs to 40-char
     Git SHA-1 commit IDs (requires the repo checkout).
  2. After the binaries are built, call finalize_frozen_spec (or pass
     --candidate-sha256 / --baseline-sha256) to embed the binary SHA-256
     digests. Aggregation requires the frozen spec.
"""

from __future__ import annotations

import argparse
from dataclasses import asdict, replace
import json
from pathlib import Path

from tools.cloud_match.spec import CloudMatchSpec, CloudSprtSpec


def prepare_spec(
    base_path: Path | str,
    output_path: Path | str,
    *,
    candidate_ref: str,
    baseline_ref: str,
    games: int,
    shards: int,
    time_control: str,
    threads: int,
    hash_mb: int,
    repo_root: Path | str | None = None,
    openings: str | None = None,
    opening_sha256: str | None = None,
    candidate_sha256: str | None = None,
    baseline_sha256: str | None = None,
    elo0: float | None = None,
    elo1: float | None = None,
) -> dict[str, list[int]]:
    base = CloudMatchSpec.from_json(base_path)
    spec = replace(
        base,
        candidate_ref=candidate_ref.strip(),
        baseline_ref=baseline_ref.strip(),
        games=games,
        shards=shards,
        time_control=time_control.strip(),
        threads=threads,
        hash_mb=hash_mb,
        openings=openings.strip() if openings is not None else base.openings,
        opening_sha256=(opening_sha256 or base.opening_sha256).lower(),
        sprt=CloudSprtSpec(
            elo0=base.sprt.elo0 if elo0 is None else elo0,
            elo1=base.sprt.elo1 if elo1 is None else elo1,
            alpha=base.sprt.alpha,
            beta=base.sprt.beta,
        ),
    )
    # Stage 1: freeze the mutable refs to resolved Git SHA-1 commit IDs.
    if repo_root is not None:
        candidate_commit, baseline_commit = spec.resolve_commits(repo_root)
        spec = spec.with_resolved_commits(candidate_commit, baseline_commit)
    # Stage 2 (optional here, normally after build): embed binary hashes.
    if candidate_sha256:
        spec = spec.with_binary_hashes(candidate_sha256.lower(), baseline_sha256.lower() if baseline_sha256 else "")
    spec.validate()
    destination = Path(output_path)
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_text(
        json.dumps(asdict(spec), indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    return {"shard": list(range(spec.shards))}


def finalize_frozen_spec(
    spec_path: Path | str,
    *,
    candidate_sha256: str,
    baseline_sha256: str,
) -> CloudMatchSpec:
    """Apply post-build binary hashes and persist the frozen spec."""
    spec = CloudMatchSpec.from_json(spec_path)
    spec = spec.with_binary_hashes(candidate_sha256.lower(), baseline_sha256.lower())
    spec.validate_frozen()
    Path(spec_path).write_text(
        json.dumps(asdict(spec), indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    return spec


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--base", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--candidate-ref", required=True)
    parser.add_argument("--baseline-ref", required=True)
    parser.add_argument("--games", type=int, required=True)
    parser.add_argument("--shards", type=int, required=True)
    parser.add_argument("--time-control", required=True)
    parser.add_argument("--threads", type=int, required=True)
    parser.add_argument("--hash-mb", type=int, required=True)
    parser.add_argument("--repo-root", type=Path)
    parser.add_argument("--openings")
    parser.add_argument("--opening-sha256")
    parser.add_argument("--candidate-sha256")
    parser.add_argument("--baseline-sha256")
    parser.add_argument("--elo0", type=float)
    parser.add_argument("--elo1", type=float)
    parser.add_argument("--github-output", type=Path)
    args = parser.parse_args()
    matrix = prepare_spec(
        args.base,
        args.output,
        candidate_ref=args.candidate_ref,
        baseline_ref=args.baseline_ref,
        games=args.games,
        shards=args.shards,
        time_control=args.time_control,
        threads=args.threads,
        hash_mb=args.hash_mb,
        repo_root=args.repo_root,
        openings=args.openings,
        opening_sha256=args.opening_sha256,
        candidate_sha256=args.candidate_sha256,
        baseline_sha256=args.baseline_sha256,
        elo0=args.elo0,
        elo1=args.elo1,
    )
    spec = CloudMatchSpec.from_json(args.output)
    encoded = json.dumps(matrix, separators=(",", ":"))
    if args.github_output:
        with args.github_output.open("a", encoding="utf-8") as stream:
            stream.write(f"matrix={encoded}\n")
            stream.write(f"experiment_id={spec.experiment_id()}\n")
            stream.write(f"candidate_commit={spec.candidate_commit}\n")
            stream.write(f"baseline_commit={spec.baseline_commit}\n")
    print(encoded)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
