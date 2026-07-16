#!/usr/bin/env python3
"""Compare Blaze perft with python-chess on deterministic legal positions."""

from __future__ import annotations

import argparse
import random
import subprocess
import sys
from pathlib import Path

import chess


def python_perft(board: chess.Board, depth: int) -> int:
    if depth == 0:
        return 1
    if depth == 1:
        return board.legal_moves.count()

    nodes = 0
    for move in board.legal_moves:
        board.push(move)
        nodes += python_perft(board, depth - 1)
        board.pop()
    return nodes


def generate_positions(count: int, seed: int) -> list[str]:
    rng = random.Random(seed)
    positions: list[str] = []
    seen: set[str] = set()

    while len(positions) < count:
        board = chess.Board()
        target_plies = rng.randint(0, 90)
        for _ in range(target_plies):
            if board.is_game_over(claim_draw=True):
                break
            board.push(rng.choice(list(board.legal_moves)))

        fen = board.fen(en_passant="fen")
        if fen not in seen:
            seen.add(fen)
            positions.append(fen)

    return positions


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--positions", type=int, default=1000)
    parser.add_argument("--depth", type=int, default=3)
    parser.add_argument("--seed", type=int, default=20260716)
    parser.add_argument(
        "--driver",
        type=Path,
        default=Path("build/blaze/perft_driver.exe"),
    )
    args = parser.parse_args()

    if args.positions <= 0 or args.depth < 0:
        parser.error("positions must be positive and depth must be non-negative")
    if not args.driver.is_file():
        print(f"perft driver not found: {args.driver}", file=sys.stderr)
        return 2

    positions = generate_positions(args.positions, args.seed)
    expected = [python_perft(chess.Board(fen), args.depth) for fen in positions]
    payload = "".join(f"{fen}\t{args.depth}\n" for fen in positions).encode("utf-8")
    result = subprocess.run(
        [str(args.driver)],
        input=payload,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    if result.returncode != 0:
        print(result.stderr.decode("utf-8", errors="replace"), file=sys.stderr)
        print(result.stdout.decode("utf-8", errors="replace"), file=sys.stderr)
        return result.returncode

    lines = result.stdout.decode("ascii").splitlines()
    if len(lines) != len(positions):
        print(
            f"driver returned {len(lines)} rows for {len(positions)} positions",
            file=sys.stderr,
        )
        return 1

    for index, (fen, expected_nodes, actual_text) in enumerate(
        zip(positions, expected, lines, strict=True),
        start=1,
    ):
        try:
            actual_nodes = int(actual_text)
        except ValueError:
            print(f"position {index}: non-numeric driver output {actual_text!r}")
            print(f"fen: {fen}")
            return 1
        if actual_nodes != expected_nodes:
            print(f"position {index}: expected {expected_nodes}, got {actual_nodes}")
            print(f"fen: {fen}")
            return 1

    print(f"{len(positions)}/{len(positions)} positions matched at depth {args.depth}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
