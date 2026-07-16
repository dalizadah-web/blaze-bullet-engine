#!/usr/bin/env python3
"""Run a comparable UCI benchmark on deterministic legal positions."""

from __future__ import annotations

import argparse
import queue
import random
import subprocess
import time
from pathlib import Path

import chess


def positions(count: int, seed: int) -> list[str]:
    rng = random.Random(seed)
    result: list[str] = []
    for _ in range(count):
        board = chess.Board()
        for _ in range(rng.randint(8, 48)):
            if board.is_game_over(claim_draw=True):
                break
            board.push(rng.choice(list(board.legal_moves)))
        result.append(board.fen(en_passant="fen"))
    return result


def run_one(
    engine: Path,
    fen: str,
    milliseconds: int,
    threads: int,
    depth_limit: int,
    nnue: Path | None,
) -> tuple[int, int, str, int]:
    process = subprocess.Popen(
        [str(engine)], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT, text=True, encoding="utf-8", bufsize=1
    )
    assert process.stdin is not None and process.stdout is not None
    lines: queue.Queue[str | None] = queue.Queue()

    def reader() -> None:
        for line in process.stdout:
            lines.put(line.rstrip("\r\n"))
        lines.put(None)

    import threading
    threading.Thread(target=reader, daemon=True).start()

    def send(command: str) -> None:
        process.stdin.write(command + "\n")
        process.stdin.flush()

    def wait_for(prefix: str, timeout: float = 5.0) -> list[str]:
        end = time.monotonic() + timeout
        observed: list[str] = []
        while time.monotonic() < end:
            try:
                line = lines.get(timeout=max(0.01, end - time.monotonic()))
            except queue.Empty:
                break
            if line is None:
                break
            observed.append(line)
            if line.startswith(prefix):
                break
        return observed

    try:
        send("uci")
        wait_for("uciok")
        send(f"setoption name Threads value {threads}")
        if nnue is not None:
            send(f"setoption name EvalFile value {nnue}")
            send("setoption name UseNNUE value true")
        send(f"position fen {fen}")
        started = time.monotonic()
        if depth_limit > 0:
            send(f"go depth {depth_limit}")
            output = wait_for("bestmove ", 60)
        else:
            send(f"go movetime {milliseconds}")
            output = wait_for("bestmove ", milliseconds / 1000 + 5)
        elapsed = int((time.monotonic() - started) * 1000)
        info_lines = [line for line in output if line.startswith("info depth")]
        info = info_lines[-1] if info_lines else ""
        fields = info.split()
        nodes = int(fields[fields.index("nodes") + 1]) if "nodes" in fields else 0
        depth = int(fields[fields.index("depth") + 1]) if "depth" in fields else 0
        best = next((line.split()[1] for line in output if line.startswith("bestmove ")), "0000")
        return depth, nodes, best, elapsed
    finally:
        if process.poll() is None:
            send("quit")
            try:
                process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--engine", type=Path, action="append", required=True)
    parser.add_argument("--positions", type=int, default=10)
    parser.add_argument("--milliseconds", type=int, default=1000)
    parser.add_argument("--seed", type=int, default=20260716)
    parser.add_argument("--threads", type=int, default=1)
    parser.add_argument("--depth", type=int, default=0)
    parser.add_argument("--nnue", type=Path, default=None)
    args = parser.parse_args()
    fens = positions(args.positions, args.seed)
    for engine in args.engine:
        print(f"engine={engine}")
        total_nodes = 0
        for index, fen in enumerate(fens, start=1):
            depth, nodes, best, elapsed = run_one(
                engine, fen, args.milliseconds, args.threads, args.depth, args.nnue)
            if chess.Move.from_uci(best) not in chess.Board(fen).legal_moves:
                raise RuntimeError(f"{engine} returned illegal move {best} on position {index}")
            total_nodes += nodes
            print(f"  {index:02d} depth={depth:2d} nodes={nodes:9d} elapsed_ms={elapsed:4d} best={best}")
        print(f"  total_nodes={total_nodes} average_nodes={total_nodes / len(fens):.1f}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
