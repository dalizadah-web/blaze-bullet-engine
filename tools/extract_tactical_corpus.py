"""Build a deduplicated tactical regression corpus from first-swing analysis."""

from __future__ import annotations

import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
from typing import Any

import chess


def motif(record: dict[str, Any]) -> str:
    if record["actual_score_cp"] <= -90_000:
        return "missed_mate_defense"
    reply = record.get("opponent_reply") or ""
    if "+" in reply or "#" in reply:
        return "king_attack"
    actual = record["actual_move"]
    if "x" in actual:
        return "unsound_capture" if "x" in reply else "hanging_piece"
    best = record.get("best_move") or ""
    if "x" in best or "+" in best or "#" in best:
        return "missed_forcing_move"
    return "tactical_other"


def pre_move_fen(record: dict[str, Any]) -> str:
    board = chess.Board(record["initial_fen"])
    ply = int(record["ply"])
    for uci in record["moves"][: ply - 1]:
        board.push_uci(uci)
    expected_color = chess.WHITE if record["candidate_color"] == "white" else chess.BLACK
    if board.turn != expected_color:
        raise ValueError(f"candidate color mismatch at ply {ply}")
    return board.fen()


def extract(summary: dict[str, Any]) -> list[dict[str, Any]]:
    positions: dict[tuple[str, str, str, str], dict[str, Any]] = {}
    for record in summary["first_swings"]:
        if not record.get("confirmed") or record.get("classification") != "tactical":
            continue
        fen = pre_move_fen(record)
        board = chess.Board(fen)
        played_uci = board.parse_san(record["actual_move"]).uci()
        best_uci = board.parse_san(record["best_move"]).uci()
        key = (fen, record["candidate_color"], record["actual_move"], record["best_move"])
        entry = positions.get(key)
        if entry is None:
            digest = hashlib.sha256("\0".join(key).encode("utf-8")).hexdigest()[:16]
            entry = {
                "id": f"sf18-first-swing-{digest}",
                "fen": fen,
                "candidate_color": record["candidate_color"],
                "played": record["actual_move"],
                "played_uci": played_uci,
                "best": record["best_move"],
                "best_uci": best_uci,
                "refutation": record.get("opponent_reply"),
                "best_score_cp": record["best_score_cp"],
                "played_score_cp": record["actual_score_cp"],
                "swing_cp": record["swing_cp"],
                "motif": motif(record),
                "occurrences": 0,
            }
            positions[key] = entry
        entry["occurrences"] += 1
        entry["swing_cp"] = max(entry["swing_cp"], record["swing_cp"])
    return sorted(positions.values(), key=lambda entry: entry["id"])


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--summary", type=Path, required=True)
    parser.add_argument("--json-output", type=Path, required=True)
    parser.add_argument("--epd-output", type=Path, required=True)
    args = parser.parse_args()

    summary = json.loads(args.summary.read_text(encoding="utf-8"))
    positions = extract(summary)
    payload = {
        "schema_version": 1,
        "source": {
            "run_id": summary["source_run_id"],
            "experiment_id": summary["experiment_id"],
            "stockfish_depth": summary["depth"],
            "swing_cp": summary["swing_cp"],
        },
        "positions": positions,
    }
    args.json_output.parent.mkdir(parents=True, exist_ok=True)
    args.json_output.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")
    args.epd_output.parent.mkdir(parents=True, exist_ok=True)
    args.epd_output.write_text(
        "".join(
            f"{' '.join(entry['fen'].split()[:4])} id \"{entry['id']}\";\n"
            for entry in positions
        ),
        encoding="utf-8",
    )
    motifs = Counter(entry["motif"] for entry in positions)
    print(json.dumps(dict(sorted(motifs.items()))))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
