"""Create a validated immutable match spec from workflow inputs."""

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
    openings: str | None = None,
    opening_sha256: str | None = None,
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
    spec.validate()
    destination = Path(output_path)
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_text(
        json.dumps(asdict(spec), indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    return {"shard": list(range(spec.shards))}


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
    parser.add_argument("--openings")
    parser.add_argument("--opening-sha256")
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
        openings=args.openings,
        opening_sha256=args.opening_sha256,
        elo0=args.elo0,
        elo1=args.elo1,
    )
    spec = CloudMatchSpec.from_json(args.output)
    encoded = json.dumps(matrix, separators=(",", ":"))
    if args.github_output:
        with args.github_output.open("a", encoding="utf-8") as stream:
            stream.write(f"matrix={encoded}\n")
            stream.write(f"experiment_id={spec.experiment_id()}\n")
    print(encoded)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
