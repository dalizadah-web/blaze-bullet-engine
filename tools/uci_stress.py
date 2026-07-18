#!/usr/bin/env python3
"""Stress Blaze's UCI lifecycle and validate every best move independently."""

from __future__ import annotations

import argparse
import queue
import random
import subprocess
import threading
import time
from pathlib import Path

import chess


def random_position(rng: random.Random) -> chess.Board:
    while True:
        board = chess.Board()
        for _ in range(rng.randint(0, 60)):
            if board.is_game_over(claim_draw=True):
                break
            board.push(rng.choice(list(board.legal_moves)))
        if not board.is_game_over(claim_draw=True):
            return board


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--engine", type=Path, required=True)
    parser.add_argument("--cycles", type=int, default=1000)
    parser.add_argument("--seed", type=int, default=20260716)
    parser.add_argument("--timeout", type=float, default=10.0)
    args = parser.parse_args()
    if not args.engine.is_file() or args.cycles <= 0:
        parser.error("engine must exist and cycles must be positive")

    process = subprocess.Popen(
        [str(args.engine)],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        encoding="utf-8",
        bufsize=1,
    )
    assert process.stdin is not None
    assert process.stdout is not None
    lines: queue.Queue[str | None] = queue.Queue()

    def read_output() -> None:
        for line in process.stdout:
            lines.put(line.rstrip("\r\n"))
        lines.put(None)

    threading.Thread(target=read_output, daemon=True).start()

    def send(command: str) -> None:
        process.stdin.write(command + "\n")
        process.stdin.flush()

    def wait_for(prefix: str) -> tuple[str, list[str]]:
        deadline = time.monotonic() + args.timeout
        observed: list[str] = []
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise RuntimeError(f"timeout waiting for {prefix!r}; output={observed!r}")
            try:
                line = lines.get(timeout=remaining)
            except queue.Empty as error:
                raise RuntimeError(f"timeout waiting for {prefix!r}; output={observed!r}") from error
            if line is None:
                raise RuntimeError(f"engine exited while waiting for {prefix!r}; output={observed!r}")
            observed.append(line)
            if line.startswith(prefix):
                return line, observed

    try:
        send("uci")
        wait_for("uciok")
        send("isready")
        wait_for("readyok")

        rng = random.Random(args.seed)
        for cycle in range(args.cycles):
            board = random_position(rng)
            send(f"position fen {board.fen(en_passant='fen')}")
            if cycle % 20 == 0:
                send("go ponder")
                send("ponderhit")
            elif cycle % 20 == 1:
                # Tournament GUIs can report a small negative remaining clock
                # when an expiry margin is enabled. The engine must still move.
                send("go wtime -22 btime -7 winc 10 binc 10")
            elif cycle % 4 == 0:
                send("go infinite")
                send("stop")
            else:
                send("go nodes 64")

            best_line, _ = wait_for("bestmove ")
            move_text = best_line.split(maxsplit=1)[1]
            try:
                move = chess.Move.from_uci(move_text)
            except ValueError as error:
                raise RuntimeError(f"cycle {cycle}: malformed bestmove {move_text!r}") from error
            if move not in board.legal_moves:
                raise RuntimeError(
                    f"cycle {cycle}: illegal bestmove {move_text} for {board.fen(en_passant='fen')}"
                )

            send("isready")
            _, readiness_output = wait_for("readyok")
            duplicates = [line for line in readiness_output if line.startswith("bestmove ")]
            if duplicates:
                raise RuntimeError(f"cycle {cycle}: duplicate bestmove output: {duplicates}")

        send("quit")
        process.wait(timeout=args.timeout)
        if process.returncode != 0:
            raise RuntimeError(f"engine exited with code {process.returncode}")
    finally:
        if process.poll() is None:
            process.kill()
            process.wait()

    print(f"{args.cycles}/{args.cycles} UCI cycles passed with legal best moves")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
