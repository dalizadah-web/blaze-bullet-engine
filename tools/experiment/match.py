"""Strict paired-match configuration, execution, and PGN validation."""

from __future__ import annotations

import argparse
from dataclasses import asdict, dataclass
import json
from pathlib import Path
import re
import subprocess
from typing import Callable

import chess.pgn

from tools.experiment.manifest import (
    ArtifactIdentity,
    ExperimentManifest,
    write_manifest,
)
from tools.experiment.pentanomial import Pentanomial, sprt_decision, sprt_llr


_SHA256_PATTERN = re.compile(r"^[0-9a-fA-F]{64}$")
_TIME_CONTROL_PATTERN = re.compile(r"^[0-9]+(?:\.[0-9]+)?\+[0-9]+(?:\.[0-9]+)?$")


@dataclass(frozen=True)
class SprtSpec:
    elo0: float
    elo1: float
    alpha: float
    beta: float


@dataclass(frozen=True)
class MatchSpec:
    schema_version: int
    name: str
    games: int
    concurrency: int
    time_control: str
    threads: int
    hash_mb: int
    repeat: bool
    opening_format: str
    openings: str
    opening_sha256: str
    opponent_sha256: str | None
    sprt: SprtSpec
    time_margin_ms: int = 150
    max_moves: int = 200
    opening_order: str = "sequential"
    opening_plies: int = 0

    @classmethod
    def from_json(cls, path: Path | str) -> "MatchSpec":
        config_path = Path(path)
        raw = json.loads(config_path.read_text(encoding="utf-8"))
        if raw.get("schema_version") != 1:
            raise ValueError("match schema_version must be 1")
        games = raw.get("games")
        if not isinstance(games, int) or games <= 0 or games % 2 != 0:
            raise ValueError("games must be a positive even number")
        concurrency = raw.get("concurrency")
        if not isinstance(concurrency, int) or concurrency <= 0 or concurrency > games:
            raise ValueError("concurrency must be between 1 and games")
        time_control = raw.get("time_control")
        if not isinstance(time_control, str) or not _TIME_CONTROL_PATTERN.fullmatch(time_control):
            raise ValueError("time_control must use base+increment seconds")
        threads = raw.get("threads")
        if not isinstance(threads, int) or not 1 <= threads <= 1024:
            raise ValueError("threads must be between 1 and 1024")
        hash_mb = raw.get("hash_mb")
        if not isinstance(hash_mb, int) or hash_mb <= 0:
            raise ValueError("hash_mb must be positive")
        if raw.get("repeat") is not True:
            raise ValueError("paired testing requires repeat=true")
        opening_format = raw.get("opening_format")
        if opening_format not in ("epd", "pgn"):
            raise ValueError("opening_format must be epd or pgn")
        opening_hash = raw.get("opening_sha256")
        if not isinstance(opening_hash, str) or not _SHA256_PATTERN.fullmatch(opening_hash):
            raise ValueError("opening_sha256 must be a SHA-256 digest")
        opponent_hash = raw.get("opponent_sha256")
        if opponent_hash is not None and (
            not isinstance(opponent_hash, str) or not _SHA256_PATTERN.fullmatch(opponent_hash)
        ):
            raise ValueError("opponent_sha256 must be null or a SHA-256 digest")
        sprt_raw = raw.get("sprt")
        if not isinstance(sprt_raw, dict):
            raise ValueError("sprt configuration is required")
        sprt = SprtSpec(
            elo0=float(sprt_raw["elo0"]),
            elo1=float(sprt_raw["elo1"]),
            alpha=float(sprt_raw["alpha"]),
            beta=float(sprt_raw["beta"]),
        )
        if sprt.elo1 <= sprt.elo0:
            raise ValueError("sprt elo1 must be greater than elo0")
        if not 0.0 < sprt.alpha < 1.0 or not 0.0 < sprt.beta < 1.0:
            raise ValueError("sprt alpha and beta must be between 0 and 1")
        name = raw.get("name")
        openings = raw.get("openings")
        if not isinstance(name, str) or not name.strip():
            raise ValueError("match name is required")
        if not isinstance(openings, str) or not openings.strip():
            raise ValueError("openings path is required")
        return cls(
            schema_version=1,
            name=name,
            games=games,
            concurrency=concurrency,
            time_control=time_control,
            threads=threads,
            hash_mb=hash_mb,
            repeat=True,
            opening_format=opening_format,
            openings=openings,
            opening_sha256=opening_hash.lower(),
            opponent_sha256=opponent_hash.lower() if opponent_hash else None,
            sprt=sprt,
            time_margin_ms=int(raw.get("time_margin_ms", 150)),
            max_moves=int(raw.get("max_moves", 200)),
            opening_order=str(raw.get("opening_order", "sequential")),
            opening_plies=int(raw.get("opening_plies", 0)),
        )


@dataclass(frozen=True)
class MatchResult:
    counts: Pentanomial
    llr: float
    decision: str
    games: int
    pgn: str
    manifest: str

    def to_json(self) -> str:
        payload = asdict(self)
        payload["counts"] = {
            "wins2": self.counts.wins2,
            "wins1_draw1": self.counts.wins1_draw1,
            "draws2": self.counts.draws2,
            "losses1_draw1": self.counts.losses1_draw1,
            "losses2": self.counts.losses2,
        }
        return json.dumps(payload, indent=2, sort_keys=True) + "\n"


def _round_key(game: chess.pgn.Game) -> tuple[int, ...]:
    text = game.headers.get("Round", "0")
    try:
        return tuple(int(part) for part in text.split("."))
    except ValueError:
        raise ValueError(f"invalid PGN Round tag: {text}") from None


def _candidate_score(game: chess.pgn.Game, candidate_name: str) -> float:
    result = game.headers.get("Result")
    if result not in ("1-0", "0-1", "1/2-1/2"):
        raise ValueError(f"game has no completed result: {result}")
    if result == "1/2-1/2":
        return 0.5
    candidate_is_white = game.headers.get("White") == candidate_name
    candidate_won = (result == "1-0") == candidate_is_white
    return 1.0 if candidate_won else 0.0


def parse_paired_pgn(
    path: Path | str,
    candidate_name: str,
    opponent_name: str,
    *,
    expected_games: int,
) -> Pentanomial:
    games: list[chess.pgn.Game] = []
    with Path(path).open(encoding="utf-8") as stream:
        while game := chess.pgn.read_game(stream):
            games.append(game)
    if len(games) != expected_games:
        raise ValueError(f"expected {expected_games} completed games, found {len(games)}")
    if len(games) % 2 != 0:
        raise ValueError("paired PGN must contain an even number of games")
    games.sort(key=_round_key)

    pair_scores: list[tuple[float, float]] = []
    expected_players = {candidate_name, opponent_name}
    for index in range(0, len(games), 2):
        first, second = games[index], games[index + 1]
        for game in (first, second):
            if {game.headers.get("White"), game.headers.get("Black")} != expected_players:
                raise ValueError("PGN game contains unexpected engine names")
        if first.headers.get("White") == second.headers.get("White"):
            raise ValueError("paired games must swap colors")
        if first.board().fen() != second.board().fen():
            raise ValueError("paired games must start from the same opening position")
        pair_scores.append(
            (_candidate_score(first, candidate_name), _candidate_score(second, candidate_name))
        )
    return Pentanomial.from_pair_scores(pair_scores)


def build_runner_command(
    spec: MatchSpec,
    *,
    runner: Path,
    candidate: Path,
    opponent: Path,
    openings: Path,
    pgn: Path,
    games: int,
    candidate_name: str,
    opponent_name: str,
) -> list[str]:
    if games <= 0 or games % 2 != 0:
        raise ValueError("games must be a positive even number")
    command = [
        str(runner),
        "-engine",
        f"name={candidate_name}",
        f"cmd={candidate}",
        "proto=uci",
        f"option.Hash={spec.hash_mb}",
        f"option.Threads={spec.threads}",
        "-engine",
        f"name={opponent_name}",
        f"cmd={opponent}",
        "proto=uci",
        f"option.Hash={spec.hash_mb}",
        f"option.Threads={spec.threads}",
        "-each",
        f"tc={spec.time_control}",
        f"timemargin={spec.time_margin_ms}",
        "-openings",
        f"file={openings}",
        f"format={spec.opening_format}",
        f"order={spec.opening_order}",
        "-games",
        str(games),
        "-repeat",
        "-concurrency",
        str(min(spec.concurrency, games)),
        "-maxmoves",
        str(spec.max_moves),
        "-pgnout",
        str(pgn),
        "fi",
    ]
    if spec.opening_plies > 0:
        opening_index = command.index("-games")
        command[opening_index:opening_index] = ["plies=" + str(spec.opening_plies)]
    return command


def run_match(
    spec: MatchSpec,
    *,
    candidate: Path | str,
    opponent: Path | str,
    runner: Path | str,
    output: Path | str,
    source_commit: str,
    candidate_name: str = "Blaze",
    opponent_name: str = "Opponent",
    games: int | None = None,
    executor: Callable[..., subprocess.CompletedProcess[str]] = subprocess.run,
) -> MatchResult:
    game_count = spec.games if games is None else games
    if game_count <= 0 or game_count % 2 != 0:
        raise ValueError("games must be a positive even number")

    candidate_identity = ArtifactIdentity.from_path(candidate)
    opponent_identity = ArtifactIdentity.from_path(opponent)
    runner_identity = ArtifactIdentity.from_path(runner)
    openings_path = Path(spec.openings)
    if not openings_path.is_absolute():
        openings_path = Path.cwd() / openings_path
    openings_identity = ArtifactIdentity.from_path(openings_path)
    if openings_identity.sha256 != spec.opening_sha256:
        raise ValueError(
            "opening SHA-256 mismatch: "
            f"expected {spec.opening_sha256}, got {openings_identity.sha256}"
        )
    if spec.opponent_sha256 and opponent_identity.sha256 != spec.opponent_sha256:
        raise ValueError(
            "opponent SHA-256 mismatch: "
            f"expected {spec.opponent_sha256}, got {opponent_identity.sha256}"
        )

    output_path = Path(output)
    if output_path.exists() and any(output_path.iterdir()):
        raise ValueError(f"experiment output directory is not empty: {output_path}")
    output_path.mkdir(parents=True, exist_ok=True)
    pgn_path = output_path / "games.pgn"
    log_path = output_path / "runner.log"
    manifest_path = output_path / "manifest.json"

    configuration = asdict(spec)
    configuration["games"] = game_count
    manifest = ExperimentManifest.create(
        experiment_id=f"{spec.name}-{source_commit[:12]}",
        source_commit=source_commit,
        candidate=candidate_identity,
        opponent=opponent_identity,
        openings=openings_identity,
        runner=runner_identity,
        configuration=configuration,
    )
    write_manifest(manifest, manifest_path)

    command = build_runner_command(
        spec,
        runner=Path(runner_identity.path),
        candidate=Path(candidate_identity.path),
        opponent=Path(opponent_identity.path),
        openings=Path(openings_identity.path),
        pgn=pgn_path.resolve(),
        games=game_count,
        candidate_name=candidate_name,
        opponent_name=opponent_name,
    )
    completed = executor(
        command,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        encoding="utf-8",
        errors="replace",
        check=False,
    )
    log_path.write_text(completed.stdout or "", encoding="utf-8", newline="\n")
    if completed.returncode != 0:
        raise RuntimeError(
            f"match runner exited with {completed.returncode}; see {log_path}"
        )
    if not pgn_path.is_file():
        raise RuntimeError(f"match runner produced no PGN: {pgn_path}")

    counts = parse_paired_pgn(
        pgn_path,
        candidate_name,
        opponent_name,
        expected_games=game_count,
    )
    llr = sprt_llr(counts, spec.sprt.elo0, spec.sprt.elo1)
    decision = sprt_decision(
        counts,
        spec.sprt.elo0,
        spec.sprt.elo1,
        spec.sprt.alpha,
        spec.sprt.beta,
    )
    result = MatchResult(
        counts=counts,
        llr=llr,
        decision=decision,
        games=game_count,
        pgn=str(pgn_path.resolve()),
        manifest=str(manifest_path.resolve()),
    )
    (output_path / "result.json").write_text(
        result.to_json(), encoding="utf-8", newline="\n"
    )
    return result


def _current_commit() -> str:
    completed = subprocess.run(
        ["git", "rev-parse", "HEAD"],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        encoding="utf-8",
        check=False,
    )
    if completed.returncode != 0:
        raise RuntimeError(f"cannot identify source commit: {completed.stderr.strip()}")
    return completed.stdout.strip()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", type=Path, required=True)
    parser.add_argument("--candidate", type=Path, required=True)
    parser.add_argument("--opponent", type=Path, required=True)
    parser.add_argument("--runner", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--games", type=int)
    parser.add_argument("--candidate-name", default="Blaze")
    parser.add_argument("--opponent-name", default="Opponent")
    args = parser.parse_args()

    result = run_match(
        MatchSpec.from_json(args.config),
        candidate=args.candidate,
        opponent=args.opponent,
        runner=args.runner,
        output=args.output,
        source_commit=_current_commit(),
        candidate_name=args.candidate_name,
        opponent_name=args.opponent_name,
        games=args.games,
    )
    print(result.to_json(), end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
