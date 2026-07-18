"""Strict paired-match configuration, execution, and PGN validation."""

from __future__ import annotations

import argparse
from dataclasses import asdict, dataclass
import json
from pathlib import Path
import re
import subprocess
from typing import Any, Callable

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
    opening_start: int = 1

    @classmethod
    def from_json(cls, path: Path | str) -> "MatchSpec":
        config_path = Path(path)
        raw = json.loads(config_path.read_text(encoding="utf-8"))
        if raw.get("schema_version") != 2:
            raise ValueError("match schema_version must be 2")
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
        opening_start = raw.get("opening_start")
        if (
            not isinstance(opening_start, int)
            or isinstance(opening_start, bool)
            or opening_start <= 0
        ):
            raise ValueError("opening_start must be a positive one-based index")
        return cls(
            schema_version=2,
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
            opening_start=opening_start,
        )


@dataclass(frozen=True)
class MatchEvidence:
    expected_games: int
    completed_games: int
    clean_games: int
    clean_pairs: int
    quarantined_games: int
    quarantined_pairs: int
    raw_wdl: dict[str, int]
    clean_wdl: dict[str, int]
    counts: Pentanomial
    termination_counts: dict[str, dict[str, int]]
    abnormal_games: tuple[dict[str, object], ...]

    def to_dict(self) -> dict[str, object]:
        return {
            "expected_games": self.expected_games,
            "completed_games": self.completed_games,
            "clean_games": self.clean_games,
            "clean_pairs": self.clean_pairs,
            "quarantined_games": self.quarantined_games,
            "quarantined_pairs": self.quarantined_pairs,
            "raw_wdl": dict(self.raw_wdl),
            "clean_wdl": dict(self.clean_wdl),
            "counts": dict(zip(
                ("wins2", "wins1_draw1", "draws2", "losses1_draw1", "losses2"),
                self.counts.as_tuple(),
                strict=True,
            )),
            "termination_counts": {
                group: dict(values) for group, values in self.termination_counts.items()
            },
            "abnormal_games": [dict(record) for record in self.abnormal_games],
        }


_COUNT_KEYS = ("wins2", "wins1_draw1", "draws2", "losses1_draw1", "losses2")
_TERMINATION_SHAPE = {
    "clean": ("ordinary", "adjudication"),
    "candidate": ("time_loss", "illegal_move", "disconnect", "stall"),
    "opponent": ("time_loss", "illegal_move", "disconnect", "stall"),
    "infrastructure_unknown": (
        "unterminated",
        "malformed",
        "unknown",
        "contradictory",
        "runner_failure",
        "paired_quarantine",
    ),
}


def _nonnegative_int(value: Any, field: str, context: str) -> int:
    if not isinstance(value, int) or isinstance(value, bool) or value < 0:
        raise ValueError(f"invalid {field} in {context}")
    return value


def _abnormal_pair_key(record: dict[str, Any], context: str) -> str:
    game_id = record.get("game_id")
    if isinstance(game_id, str) and game_id.endswith(("-w", "-b")):
        return game_id[:-2]
    game_index = record.get("game_index")
    if isinstance(game_index, int) and not isinstance(game_index, bool) and game_index >= 0:
        return f"index-{game_index // 2}"
    raise ValueError(f"abnormal game has no auditable pair identity in {context}")


def _semantic_category_and_offender(record: dict[str, Any]) -> tuple[str, str]:
    termination = str(record.get("termination", "")).casefold().strip()
    result = record.get("result")
    candidate_color = record.get("candidate_color")
    reason = str(record.get("reason", ""))
    engine_categories = {
        "time forfeit": "time_loss",
        "illegal move": "illegal_move",
        "abandoned": "disconnect",
        "stalled connection": "stall",
    }
    if termination in engine_categories:
        if result not in ("1-0", "0-1") or candidate_color not in ("white", "black"):
            return "contradictory", "unknown"
        candidate_lost = (candidate_color == "white" and result == "0-1") or (
            candidate_color == "black" and result == "1-0"
        )
        return engine_categories[termination], "candidate" if candidate_lost else "opponent"
    if termination == "unterminated":
        return "unterminated", "unknown"
    if termination not in ("", "adjudication"):
        return "unknown", "unknown"
    if reason.startswith(("match runner exited with ", "match runner produced no PGN")):
        return "runner_failure", "unknown"
    if reason == "color-paired game was quarantined":
        if result in ("1-0", "0-1", "1/2-1/2"):
            return "paired_quarantine", "unknown"
        return "unterminated", "unknown"
    if reason and reason not in ("game has no completed result", "PGN game is missing"):
        return "malformed", "unknown"
    if result not in ("1-0", "0-1", "1/2-1/2"):
        return "unterminated", "unknown"
    return "unknown", "unknown"


def validate_evidence_payload(
    payload: dict[str, Any], *, context: str, expected_games: int | None = None
) -> MatchEvidence:
    """Validate schema-v3 evidence without trusting producer arithmetic."""
    if payload.get("schema_version") != 3:
        raise ValueError(f"unsupported evidence schema in {context}")
    expected = _nonnegative_int(payload.get("expected_games"), "expected_games", context)
    if expected == 0 or expected % 2:
        raise ValueError(f"expected_games must be positive and even in {context}")
    if expected_games is not None and expected != expected_games:
        raise ValueError(f"game count mismatch in {context}")
    completed = _nonnegative_int(payload.get("completed_games"), "completed_games", context)
    clean_games = _nonnegative_int(payload.get("clean_games"), "clean_games", context)
    clean_pairs = _nonnegative_int(payload.get("clean_pairs"), "clean_pairs", context)
    quarantined_games = _nonnegative_int(
        payload.get("quarantined_games"), "quarantined_games", context
    )
    quarantined_pairs = _nonnegative_int(
        payload.get("quarantined_pairs"), "quarantined_pairs", context
    )
    if completed > expected:
        raise ValueError(f"completed game count exceeds expected games in {context}")
    if clean_games != clean_pairs * 2 or quarantined_games != quarantined_pairs * 2:
        raise ValueError(f"pair/game arithmetic mismatch in {context}")
    if clean_games + quarantined_games != expected:
        raise ValueError(f"clean/quarantined coverage mismatch in {context}")

    raw = payload.get("raw_wdl")
    if not isinstance(raw, dict) or set(raw) != {"wins", "draws", "losses"}:
        raise ValueError(f"invalid raw_wdl in {context}")
    raw_wdl = {
        key: _nonnegative_int(raw[key], f"raw_wdl.{key}", context)
        for key in ("wins", "draws", "losses")
    }
    if sum(raw_wdl.values()) != completed:
        raise ValueError(f"raw W/D/L does not match completed games in {context}")
    raw_clean = payload.get("clean_wdl")
    if not isinstance(raw_clean, dict) or set(raw_clean) != {"wins", "draws", "losses"}:
        raise ValueError(f"invalid clean_wdl in {context}")
    clean_wdl = {
        key: _nonnegative_int(raw_clean[key], f"clean_wdl.{key}", context)
        for key in ("wins", "draws", "losses")
    }
    if sum(clean_wdl.values()) != clean_games:
        raise ValueError(f"clean W/D/L does not match clean games in {context}")

    raw_counts = payload.get("counts")
    if not isinstance(raw_counts, dict) or set(raw_counts) != set(_COUNT_KEYS):
        raise ValueError(f"invalid pentanomial counts in {context}")
    count_values = [
        _nonnegative_int(raw_counts[key], f"counts.{key}", context)
        for key in _COUNT_KEYS
    ]
    counts = Pentanomial(*count_values)
    if counts.pairs != clean_pairs:
        raise ValueError(f"pentanomial pair count mismatch in {context}")
    base_wins = counts.wins2 * 2 + counts.wins1_draw1
    base_draws = counts.wins1_draw1 + counts.draws2 * 2 + counts.losses1_draw1
    base_losses = counts.losses2 * 2 + counts.losses1_draw1
    mixed_draw_pairs = clean_wdl["wins"] - base_wins
    if (
        mixed_draw_pairs < 0
        or mixed_draw_pairs > counts.draws2
        or clean_wdl["losses"] - base_losses != mixed_draw_pairs
        or clean_wdl["draws"] != base_draws - mixed_draw_pairs * 2
    ):
        raise ValueError(f"clean W/D/L contradicts pentanomial counts in {context}")

    raw_terminations = payload.get("termination_counts")
    if not isinstance(raw_terminations, dict) or set(raw_terminations) != set(_TERMINATION_SHAPE):
        raise ValueError(f"invalid termination counts in {context}")
    termination_counts: dict[str, dict[str, int]] = {}
    for group, keys in _TERMINATION_SHAPE.items():
        values = raw_terminations.get(group)
        if not isinstance(values, dict) or set(values) != set(keys):
            raise ValueError(f"invalid termination counts for {group} in {context}")
        termination_counts[group] = {
            key: _nonnegative_int(values[key], f"termination_counts.{group}.{key}", context)
            for key in keys
        }

    records = payload.get("abnormal_games")
    if not isinstance(records, list) or any(not isinstance(record, dict) for record in records):
        raise ValueError(f"invalid abnormal game records in {context}")
    abnormal_games = tuple(dict(record) for record in records)
    abnormal_total = sum(
        sum(values.values())
        for group, values in termination_counts.items()
        if group != "clean"
    )
    if abnormal_total != len(abnormal_games):
        raise ValueError(f"termination count does not match abnormal records in {context}")
    if sum(termination_counts["clean"].values()) + abnormal_total != expected:
        raise ValueError(f"termination coverage does not match expected games in {context}")

    observed_counts = {
        group: {key: 0 for key in keys}
        for group, keys in _TERMINATION_SHAPE.items()
        if group != "clean"
    }
    pair_keys: set[str] = set()
    seen_record_ids: set[tuple[str, str]] = set()
    abnormal_wdl = {"wins": 0, "draws": 0, "losses": 0}
    completed_abnormal_games = 0
    for record in abnormal_games:
        offender = record.get("offender")
        category = record.get("category")
        semantic_category, semantic_offender = _semantic_category_and_offender(record)
        if category != semantic_category or offender != semantic_offender:
            raise ValueError(f"record contradicts raw termination semantics in {context}")
        group = offender if offender in ("candidate", "opponent") else "infrastructure_unknown"
        if not isinstance(category, str) or category not in observed_counts[group]:
            raise ValueError(f"invalid abnormal termination category in {context}")
        if group in ("candidate", "opponent"):
            candidate_color = record.get("candidate_color")
            result = record.get("result")
            if candidate_color not in ("white", "black") or result not in ("1-0", "0-1"):
                raise ValueError(f"contradictory offender/result evidence in {context}")
            candidate_lost = (candidate_color == "white" and result == "0-1") or (
                candidate_color == "black" and result == "1-0"
            )
            if (group == "candidate") != candidate_lost:
                raise ValueError(f"contradictory offender/result evidence in {context}")
            expected_termination = {
                "time_loss": "time forfeit",
                "illegal_move": "illegal move",
                "disconnect": "abandoned",
                "stall": "stalled connection",
            }[category]
            termination = record.get("termination")
            if not isinstance(termination, str) or termination.casefold().strip() != expected_termination:
                raise ValueError(f"termination/category mismatch in {context}")
        observed_counts[group][category] += 1
        result = record.get("result")
        candidate_color = record.get("candidate_color")
        game_id = record.get("game_id")
        if isinstance(game_id, str) and game_id.endswith(("-w", "-b")):
            suffix_color = "white" if game_id.endswith("-w") else "black"
            if candidate_color in ("white", "black") and candidate_color != suffix_color:
                raise ValueError(f"game ID suffix contradicts candidate color in {context}")
            if result in ("1-0", "0-1", "1/2-1/2") and candidate_color != suffix_color:
                raise ValueError(f"game ID suffix lacks completed-game candidate color in {context}")
        if result == "1/2-1/2":
            abnormal_wdl["draws"] += 1
            completed_abnormal_games += 1
        elif result in ("1-0", "0-1"):
            if candidate_color not in ("white", "black"):
                raise ValueError(f"completed abnormal game has unknown candidate color in {context}")
            candidate_won = (result == "1-0") == (candidate_color == "white")
            abnormal_wdl["wins" if candidate_won else "losses"] += 1
            completed_abnormal_games += 1
        elif result != "*":
            raise ValueError(f"invalid abnormal game result in {context}")
        pair_key = _abnormal_pair_key(record, context)
        game_identity = str(record.get("game_id", record.get("game_index")))
        identity = (pair_key, game_identity)
        if identity in seen_record_ids:
            raise ValueError(f"duplicate abnormal game record in {context}")
        seen_record_ids.add(identity)
        pair_keys.add(pair_key)
        for key in ("round", "result", "termination", "reason"):
            if not isinstance(record.get(key), str):
                raise ValueError(f"invalid abnormal game {key} in {context}")
    if observed_counts != {
        group: termination_counts[group] for group in observed_counts
    }:
        raise ValueError(f"termination count does not match abnormal categories in {context}")
    if len(pair_keys) != quarantined_pairs:
        raise ValueError(f"abnormal records do not cover quarantined pairs in {context}")
    if len(abnormal_games) != quarantined_games:
        raise ValueError(f"abnormal records do not cover quarantined games in {context}")
    if completed != clean_games + completed_abnormal_games:
        raise ValueError(f"completed games do not reconcile with evidence in {context}")
    if any(
        raw_wdl[key] != clean_wdl[key] + abnormal_wdl[key]
        for key in raw_wdl
    ):
        raise ValueError(f"raw W/D/L does not reconcile with clean and abnormal evidence in {context}")

    return MatchEvidence(
        expected_games=expected,
        completed_games=completed,
        clean_games=clean_games,
        clean_pairs=clean_pairs,
        quarantined_games=quarantined_games,
        quarantined_pairs=quarantined_pairs,
        raw_wdl=raw_wdl,
        clean_wdl=clean_wdl,
        counts=counts,
        termination_counts=termination_counts,
        abnormal_games=abnormal_games,
    )


@dataclass(frozen=True)
class MatchResult:
    evidence: MatchEvidence
    llr: float
    decision: str
    pgn: str
    manifest: str

    @property
    def counts(self) -> Pentanomial:
        return self.evidence.counts

    @property
    def games(self) -> int:
        return self.evidence.expected_games

    def to_json(self) -> str:
        payload = {
            "schema_version": 3,
            **self.evidence.to_dict(),
            "llr": self.llr,
            "decision": self.decision,
            "pgn": self.pgn,
            "manifest": self.manifest,
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


def _empty_termination_counts() -> dict[str, dict[str, int]]:
    engine_categories = {
        "time_loss": 0,
        "illegal_move": 0,
        "disconnect": 0,
        "stall": 0,
    }
    return {
        "clean": {"ordinary": 0, "adjudication": 0},
        "candidate": dict(engine_categories),
        "opponent": dict(engine_categories),
        "infrastructure_unknown": {
            "unterminated": 0,
            "malformed": 0,
            "unknown": 0,
            "contradictory": 0,
            "runner_failure": 0,
            "paired_quarantine": 0,
        },
    }


def _losing_engine(
    game: chess.pgn.Game, candidate_name: str, opponent_name: str
) -> str | None:
    result = game.headers.get("Result")
    if result == "1-0":
        loser = game.headers.get("Black")
    elif result == "0-1":
        loser = game.headers.get("White")
    else:
        return None
    if loser == candidate_name:
        return "candidate"
    if loser == opponent_name:
        return "opponent"
    return None


def _termination_record(
    game: chess.pgn.Game,
    candidate_name: str,
    opponent_name: str,
    game_index: int,
) -> tuple[str | None, dict[str, object] | None]:
    result = game.headers.get("Result", "*")
    raw_termination = game.headers.get("Termination", "").strip()
    termination = raw_termination.casefold()
    base = {
        "game_index": game_index,
        "game_id": f"game-{game_index + 1:06d}",
        "round": game.headers.get("Round", ""),
        "result": result,
        "termination": raw_termination,
        "candidate_color": (
            "white" if game.headers.get("White") == candidate_name else "black"
        ),
    }
    engine_categories = {
        "time forfeit": "time_loss",
        "illegal move": "illegal_move",
        "abandoned": "disconnect",
        "stalled connection": "stall",
    }
    if termination in engine_categories:
        offender = _losing_engine(game, candidate_name, opponent_name)
        if offender is None:
            return None, {
                **base,
                "category": "contradictory",
                "offender": "unknown",
                "reason": "engine-failure termination has no decisive loser",
            }
        return None, {
            **base,
            "category": engine_categories[termination],
            "offender": offender,
            "reason": "engine failure",
        }
    if termination == "unterminated":
        return None, {
            **base,
            "category": "unterminated",
            "offender": "unknown",
            "reason": "runner marked the game unterminated",
        }
    if termination not in ("", "adjudication"):
        return None, {
            **base,
            "category": "unknown",
            "offender": "unknown",
            "reason": "unrecognized termination",
        }
    if game.errors:
        return None, {
            **base,
            "category": "malformed",
            "offender": "unknown",
            "reason": "; ".join(str(error) for error in game.errors),
        }
    if result not in ("1-0", "0-1", "1/2-1/2"):
        return None, {
            **base,
            "category": "unterminated",
            "offender": "unknown",
            "reason": "game has no completed result",
        }
    if not termination:
        return "ordinary", None
    if termination == "adjudication":
        return "adjudication", None
    return None, {
        **base,
        "category": "unknown",
        "offender": "unknown",
        "reason": "unrecognized termination",
    }


class _StrictResultBuilder(chess.pgn.GameBuilder):
    def begin_game(self) -> None:
        super().begin_game()
        self.movetext_result: str | None = None
        self.header_result: str | None = None

    def visit_header(self, tagname: str, tagvalue: str) -> None:
        if tagname == "Result":
            self.header_result = tagvalue
        super().visit_header(tagname, tagvalue)

    def visit_result(self, result: str) -> None:
        self.movetext_result = result
        super().visit_result(result)

    def result(self) -> chess.pgn.Game:
        game = super().result()
        setattr(game, "_blaze_movetext_result", self.movetext_result)
        setattr(game, "_blaze_header_result", self.header_result)
        return game


def _read_pgn_slots(
    path: Path | str,
    expected_games: int,
    candidate_name: str,
    opponent_name: str,
) -> list[chess.pgn.Game | None]:
    games: list[chess.pgn.Game] = []
    with Path(path).open(encoding="utf-8") as stream:
        while game := chess.pgn.read_game(stream, Visitor=_StrictResultBuilder):
            games.append(game)
    if len(games) > expected_games:
        raise ValueError(f"PGN contains more than two games per expected round")
    slots: list[chess.pgn.Game | None] = [None] * expected_games
    for game in games:
        round_key = _round_key(game)
        expected_pairs = expected_games // 2
        if len(round_key) != 1 or not 1 <= round_key[0] <= expected_pairs:
            raise ValueError("PGN requires integer Round values in the expected pair range")
        if {game.headers.get("White"), game.headers.get("Black")} != {
            candidate_name,
            opponent_name,
        }:
            raise ValueError("PGN game contains unexpected engine names")
        candidate_is_white = game.headers.get("White") == candidate_name
        slot = (round_key[0] - 1) * 2 + (0 if candidate_is_white else 1)
        if slots[slot] is not None:
            color = "white" if candidate_is_white else "black"
            raise ValueError(
                f"duplicate candidate color {color} in PGN round {round_key[0]}"
            )
        header_result = getattr(game, "_blaze_header_result", None)
        movetext_result = getattr(game, "_blaze_movetext_result", None)
        if header_result is None:
            raise ValueError(f"PGN round {round_key[0]} has no Result header")
        if not game.errors and movetext_result != header_result:
            raise ValueError(
                f"PGN header result {header_result!r} disagrees with movetext result "
                f"{movetext_result!r} in round {round_key[0]}"
            )
        slots[slot] = game
    return slots


def parse_paired_pgn(
    path: Path | str,
    candidate_name: str,
    opponent_name: str,
    *,
    expected_games: int,
) -> MatchEvidence:
    slots = _read_pgn_slots(path, expected_games, candidate_name, opponent_name)

    pair_scores: list[tuple[float, float]] = []
    raw_wdl = {"wins": 0, "draws": 0, "losses": 0}
    clean_wdl = {"wins": 0, "draws": 0, "losses": 0}
    termination_counts = _empty_termination_counts()
    abnormal_games: list[dict[str, object]] = []
    completed_games = 0
    clean_pairs = 0
    for index in range(0, expected_games, 2):
        first, second = slots[index], slots[index + 1]
        if first is not None and second is not None:
            if first.headers.get("White") == second.headers.get("White"):
                raise ValueError("paired games must swap colors")
            if first.board().fen() != second.board().fen():
                raise ValueError("paired games must start from the same opening position")
        entries: list[tuple[int, chess.pgn.Game | None, float | None, str | None, dict[str, object] | None]] = []
        for game_index, game in ((index, first), (index + 1, second)):
            if game is None:
                abnormal = {
                    "game_index": game_index,
                    "game_id": f"game-{game_index + 1:06d}",
                    "round": str(game_index // 2 + 1),
                    "result": "*",
                    "termination": "",
                    "candidate_color": "white" if game_index % 2 == 0 else "black",
                    "category": "unterminated",
                    "offender": "unknown",
                    "reason": "PGN game is missing",
                }
                entries.append((game_index, None, None, None, abnormal))
                continue
            result = game.headers.get("Result")
            score = None
            if result in ("1-0", "0-1", "1/2-1/2"):
                score = _candidate_score(game, candidate_name)
                completed_games += 1
                raw_wdl[("wins" if score == 1.0 else "draws" if score == 0.5 else "losses")] += 1
            clean_kind, abnormal = _termination_record(
                game, candidate_name, opponent_name, game_index
            )
            entries.append((game_index, game, score, clean_kind, abnormal))
        pair_is_clean = all(clean_kind is not None for _, _, _, clean_kind, _ in entries)
        if pair_is_clean:
            scores = [score for _, _, score, _, _ in entries]
            if any(score is None for score in scores):
                raise AssertionError("clean pair does not have two completed results")
            pair_scores.append((float(scores[0]), float(scores[1])))
            clean_pairs += 1
            for _, _, score, clean_kind, _ in entries:
                assert score is not None and clean_kind is not None
                termination_counts["clean"][clean_kind] += 1
                clean_wdl[("wins" if score == 1.0 else "draws" if score == 0.5 else "losses")] += 1
        else:
            for game_index, game, _, clean_kind, abnormal in entries:
                if clean_kind is not None:
                    assert game is not None
                    abnormal = {
                        "game_index": game_index,
                        "game_id": f"game-{game_index + 1:06d}",
                        "round": game.headers.get("Round", ""),
                        "result": game.headers.get("Result", "*"),
                        "termination": game.headers.get("Termination", "").strip(),
                        "candidate_color": (
                            "white" if game.headers.get("White") == candidate_name else "black"
                        ),
                        "category": "paired_quarantine",
                        "offender": "unknown",
                        "reason": "color-paired game was quarantined",
                    }
                assert abnormal is not None
                abnormal_games.append(abnormal)
                offender = str(abnormal["offender"])
                category = str(abnormal["category"])
                group = offender if offender in ("candidate", "opponent") else "infrastructure_unknown"
                termination_counts[group][category] += 1
    total_pairs = expected_games // 2
    quarantined_pairs = total_pairs - clean_pairs
    return MatchEvidence(
        expected_games=expected_games,
        completed_games=completed_games,
        clean_games=clean_pairs * 2,
        clean_pairs=clean_pairs,
        quarantined_games=quarantined_pairs * 2,
        quarantined_pairs=quarantined_pairs,
        raw_wdl=raw_wdl,
        clean_wdl=clean_wdl,
        counts=Pentanomial.from_pair_scores(pair_scores),
        termination_counts=termination_counts,
        abnormal_games=tuple(abnormal_games),
    )


def _runner_failure_evidence(
    expected_games: int,
    reason: str,
    partial: MatchEvidence | None = None,
    slots: list[chess.pgn.Game | None] | None = None,
    candidate_name: str = "Blaze",
    opponent_name: str = "Opponent",
) -> MatchEvidence:
    termination_counts = _empty_termination_counts()
    records: list[dict[str, object]] = []
    for index in range(expected_games):
        game = slots[index] if slots is not None else None
        preserved = None
        if game is not None:
            _, typed = _termination_record(game, candidate_name, opponent_name, index)
            if typed is not None:
                preserved = typed
        if preserved is not None:
            record = preserved
        else:
            record = {
                "game_index": index,
                "game_id": f"game-{index + 1:06d}",
                "round": (
                    game.headers.get("Round", "")
                    if game is not None
                    else str(index // 2 + 1)
                ),
                "result": game.headers.get("Result", "*") if game is not None else "*",
                "termination": (
                    game.headers.get("Termination", "").strip()
                    if game is not None
                    else ""
                ),
                "candidate_color": (
                    "white"
                    if game is not None and game.headers.get("White") == candidate_name
                    else "black"
                    if game is not None and game.headers.get("Black") == candidate_name
                    else "white"
                    if index % 2 == 0
                    else "black"
                ),
                "category": "runner_failure",
                "offender": "unknown",
                "reason": reason,
            }
        records.append(record)
        offender = str(record["offender"])
        category = str(record["category"])
        group = offender if offender in ("candidate", "opponent") else "infrastructure_unknown"
        termination_counts[group][category] += 1
    return MatchEvidence(
        expected_games=expected_games,
        completed_games=partial.completed_games if partial is not None else 0,
        clean_games=0,
        clean_pairs=0,
        quarantined_games=expected_games,
        quarantined_pairs=expected_games // 2,
        raw_wdl=(
            dict(partial.raw_wdl)
            if partial is not None
            else {"wins": 0, "draws": 0, "losses": 0}
        ),
        clean_wdl={"wins": 0, "draws": 0, "losses": 0},
        counts=Pentanomial(0, 0, 0, 0, 0),
        termination_counts=termination_counts,
        abnormal_games=tuple(records),
    )


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
        f"start={spec.opening_start}",
        "-games",
        "2",
        "-rounds",
        str(games // 2),
        "-repeat",
        "-recover",
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
    if spec.opening_format == "epd":
        opening_count = sum(
            bool(line.strip())
            for line in openings_path.read_text(encoding="utf-8-sig").splitlines()
        )
        last_opening = spec.opening_start + game_count // 2 - 1
        if last_opening > opening_count:
            raise ValueError(
                "opening range exceeds source without wraparound: "
                f"requested {spec.opening_start}..{last_opening}, source has {opening_count}"
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
    configuration["opening_count"] = game_count // 2
    if spec.opening_format == "epd":
        configuration["opening_suite_positions"] = opening_count
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
        reason = f"match runner exited with {completed.returncode}"
        partial = None
        slots = None
        if pgn_path.is_file():
            try:
                slots = _read_pgn_slots(
                    pgn_path, game_count, candidate_name, opponent_name
                )
                partial = parse_paired_pgn(
                    pgn_path,
                    candidate_name,
                    opponent_name,
                    expected_games=game_count,
                )
            except (OSError, ValueError):
                partial = None
                slots = None
        evidence = _runner_failure_evidence(
            game_count,
            reason,
            partial,
            slots,
            candidate_name,
            opponent_name,
        )
        pgn_path.touch(exist_ok=True)
    elif not pgn_path.is_file():
        evidence = _runner_failure_evidence(
            game_count, "match runner produced no PGN"
        )
        pgn_path.touch()
    else:
        evidence = parse_paired_pgn(
            pgn_path,
            candidate_name,
            opponent_name,
            expected_games=game_count,
        )
    evidence = validate_evidence_payload(
        {"schema_version": 3, **evidence.to_dict()},
        context="local match result",
        expected_games=game_count,
    )
    if evidence.clean_pairs == 0:
        llr = 0.0
        decision = "no_clean_pairs"
    else:
        llr = sprt_llr(evidence.counts, spec.sprt.elo0, spec.sprt.elo1)
        decision = sprt_decision(
            evidence.counts,
            spec.sprt.elo0,
            spec.sprt.elo1,
            spec.sprt.alpha,
            spec.sprt.beta,
        )
    result = MatchResult(
        evidence=evidence,
        llr=llr,
        decision=decision,
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
