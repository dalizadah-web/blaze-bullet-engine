"""Classify the first major Stockfish-evaluated regression in Candidate losses."""

from __future__ import annotations

import argparse
from collections import Counter
import json
from pathlib import Path
import re
from typing import Any

import chess
import chess.engine
import chess.pgn


_ENGINE: chess.engine.SimpleEngine | None = None
_DEPTH = 14
_SWING_CP = 150
_SCORE = re.compile(r"([+-])(?:(M)(\d+)|(\d+(?:\.\d+)?))/")


def _close_engine() -> None:
    if _ENGINE is not None:
        try:
            _ENGINE.quit()
        except (chess.engine.EngineError, OSError):
            pass


def _configure_worker(engine_path: str, depth: int, swing_cp: int) -> None:
    global _DEPTH, _ENGINE, _SWING_CP
    _DEPTH = depth
    _SWING_CP = swing_cp
    _ENGINE = chess.engine.SimpleEngine.popen_uci(engine_path)
    _ENGINE.configure({"Threads": 1, "Hash": 16})


def _score(info: dict[str, Any], color: chess.Color) -> int:
    return info["score"].pov(color).score(mate_score=100_000)


def _is_forcing(board: chess.Board, move: chess.Move | None) -> bool:
    return bool(move) and (
        board.is_capture(move) or move.promotion is not None or board.gives_check(move)
    )


def _is_endgame(board: chess.Board) -> bool:
    non_pawn_pieces = sum(
        len(board.pieces(piece, color))
        for color in chess.COLORS
        for piece in (chess.KNIGHT, chess.BISHOP, chess.ROOK, chess.QUEEN)
    )
    return not board.queens and non_pawn_pieces <= 4


def _classify(
    board: chess.Board,
    actual: chess.Move,
    best: chess.Move | None,
    reply: chess.Move | None,
) -> str:
    if _is_endgame(board):
        return "endgame"
    if _is_forcing(board, actual) or _is_forcing(board, best) or _is_forcing(board, reply):
        return "tactical"
    return "static_evaluation"


def _comment_score(comment: str, candidate_color: chess.Color) -> int | None:
    match = _SCORE.search(comment)
    if not match:
        return None
    sign = 1 if match.group(1) == "+" else -1
    if match.group(2):
        value = 100_000 - int(match.group(3))
    else:
        value = round(float(match.group(4)) * 100)
    white_score = sign * value
    return white_score if candidate_color == chess.WHITE else -white_score


def _loss_records(pgn_root: Path, swing_cp: int) -> list[dict[str, Any]]:
    records: list[dict[str, Any]] = []
    for path in sorted(pgn_root.rglob("*.pgn")):
        with path.open(encoding="utf-8-sig") as stream:
            game_index = 0
            while game := chess.pgn.read_game(stream):
                game_index += 1
                candidate_color = (
                    chess.WHITE if game.headers.get("White") == "Candidate" else chess.BLACK
                )
                candidate_result = (
                    game.headers.get("Result") == "0-1"
                    if candidate_color == chess.WHITE
                    else game.headers.get("Result") == "1-0"
                )
                if not candidate_result:
                    continue
                board = game.board()
                initial_fen = board.fen()
                moves: list[str] = []
                candidate_scores: list[tuple[int, int]] = []
                for ply, node in enumerate(game.mainline(), start=1):
                    move = node.move
                    if board.turn == candidate_color:
                        score = _comment_score(node.comment, candidate_color)
                        if score is not None:
                            candidate_scores.append((ply, score))
                    moves.append(move.uci())
                    board.push(move)
                signals = [
                    {
                        "ply": ply,
                        "recorded_swing_cp": previous_score - score,
                    }
                    for (_previous_ply, previous_score), (ply, score) in zip(
                        candidate_scores, candidate_scores[1:]
                    )
                    if previous_score - score >= swing_cp
                ]
                records.append(
                    {
                        "source": str(path),
                        "game_index": game_index,
                        "initial_fen": initial_fen,
                        "moves": moves,
                        "candidate_color": candidate_color,
                        "signals": signals[:1],
                    }
                )
    return records


def _analyse_loss(record: dict[str, Any]) -> dict[str, Any]:
    if _ENGINE is None:
        raise RuntimeError("Stockfish worker was not initialized")
    board = chess.Board(record["initial_fen"])
    candidate_color = chess.Color(record["candidate_color"])
    target_signals = {signal["ply"]: signal for signal in record["signals"]}
    if not target_signals:
        return {
            **record,
            "classification": "static_evaluation",
            "confirmed": False,
            "reason": "no recorded candidate-evaluation swing met the threshold",
        }
    for ply, uci in enumerate(record["moves"], start=1):
        actual = chess.Move.from_uci(uci)
        if board.turn != candidate_color:
            board.push(actual)
            continue
        if ply not in target_signals:
            board.push(actual)
            continue

        before = _ENGINE.analyse(board, chess.engine.Limit(depth=_DEPTH))
        before_score = _score(before, candidate_color)
        best_line = before.get("pv", [])
        best = best_line[0] if best_line else None
        best_san = board.san(best) if best else None
        position_before_move = board.copy(stack=False)
        actual_san = board.san(actual)
        board.push(actual)
        after = _ENGINE.analyse(board, chess.engine.Limit(depth=_DEPTH))
        after_score = _score(after, candidate_color)
        swing = before_score - after_score
        if swing < _SWING_CP:
            continue
        reply_line = after.get("pv", [])
        reply = reply_line[0] if reply_line else None
        return {
            **record,
            "classification": _classify(position_before_move, actual, best, reply),
            "confirmed": True,
            "recorded_swing_cp": target_signals[ply]["recorded_swing_cp"],
            "candidate_color": "white" if candidate_color else "black",
            "move_number": board.fullmove_number - (1 if candidate_color == chess.BLACK else 0),
            "ply": ply,
            "actual_move": actual_san,
            "best_move": best_san,
            "best_score_cp": before_score,
            "actual_score_cp": after_score,
            "swing_cp": swing,
            "opponent_reply": board.san(reply) if reply else None,
        }
    return {
        **record,
        "classification": "static_evaluation",
        "confirmed": False,
        "reason": "Stockfish did not confirm a nominated sharp regression",
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pgn-root", type=Path, required=True)
    parser.add_argument("--stockfish", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--depth", type=int, default=14)
    parser.add_argument("--swing-cp", type=int, default=150)
    args = parser.parse_args()

    stockfish = args.stockfish.resolve()
    if not stockfish.is_file():
        parser.error(f"Stockfish binary does not exist: {stockfish}")
    records = _loss_records(args.pgn_root, args.swing_cp)
    results: list[dict[str, Any]] = []
    _configure_worker(str(stockfish), args.depth, args.swing_cp)
    try:
        for completed, record in enumerate(records, start=1):
            results.append(_analyse_loss(record))
            if completed % 100 == 0 or completed == len(records):
                print(f"analysed {completed}/{len(records)} losses")
    finally:
        _close_engine()

    results.sort(key=lambda item: (item["source"], item["game_index"]))
    categories = Counter(item["classification"] for item in results)
    payload = {
        "pgn_root": str(args.pgn_root.resolve()),
        "stockfish": str(stockfish),
        "depth": args.depth,
        "swing_cp": args.swing_cp,
        "losses": len(results),
        "categories": dict(sorted(categories.items())),
        "first_swings": results,
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(payload["categories"], sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
