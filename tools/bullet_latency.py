#!/usr/bin/env python3
"""Measure persistent-process move latency across Blaze's bullet matrix."""

from __future__ import annotations

import argparse
from dataclasses import dataclass
import json
import math
from pathlib import Path
import queue
import subprocess
import threading
import time

import chess

if __package__:
    from tools.benchmark import positions, sha256
else:
    from benchmark import positions, sha256


@dataclass(frozen=True)
class TimeControl:
    base_ms: int
    increment_ms: int

    @classmethod
    def parse(cls, text: str) -> "TimeControl":
        parts = text.split("+")
        if len(parts) != 2:
            raise ValueError(f"invalid time control: {text}")
        try:
            base_seconds = float(parts[0])
            increment_seconds = float(parts[1])
        except ValueError as error:
            raise ValueError(f"invalid time control: {text}") from error
        if not math.isfinite(base_seconds) or not math.isfinite(increment_seconds):
            raise ValueError(f"invalid time control: {text}")
        if base_seconds < 0 or increment_seconds < 0:
            raise ValueError(f"invalid time control: {text}")
        base_ms = round(base_seconds * 1000)
        increment_ms = round(increment_seconds * 1000)
        if base_ms == 0 and increment_ms == 0:
            raise ValueError("time control must provide time or increment")
        return cls(base_ms, increment_ms)

    def __str__(self) -> str:
        return f"{self.base_ms / 1000:g}+{self.increment_ms / 1000:g}"


def percentile(samples: list[float], quantile: float) -> float:
    if not samples:
        raise ValueError("percentile requires samples")
    if not 0 < quantile <= 1:
        raise ValueError("quantile must be in (0, 1]")
    ordered = sorted(samples)
    rank = max(1, math.ceil(quantile * len(ordered)))
    return ordered[rank - 1]


def uci_go_command(control: TimeControl, white_to_move: bool) -> str:
    del white_to_move  # Both sides receive identical synthetic clocks.
    return (
        f"go wtime {control.base_ms} btime {control.base_ms} "
        f"winc {control.increment_ms} binc {control.increment_ms}"
    )


class PersistentUciEngine:
    def __init__(
        self,
        executable: Path,
        *,
        threads: int,
        hash_mb: int,
        move_overhead_ms: int,
    ) -> None:
        self.process = subprocess.Popen(
            [str(executable)],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            encoding="utf-8",
            bufsize=1,
        )
        if self.process.stdin is None or self.process.stdout is None:
            raise RuntimeError("failed to open engine pipes")
        self.lines: queue.Queue[str | None] = queue.Queue()
        threading.Thread(target=self._read_output, daemon=True).start()
        self.send("uci")
        self.wait_for("uciok", 5.0)
        self.send(f"setoption name Threads value {threads}")
        self.send(f"setoption name Hash value {hash_mb}")
        self.send(f"setoption name Move Overhead value {move_overhead_ms}")
        self.send("isready")
        self.wait_for("readyok", 5.0)

    def _read_output(self) -> None:
        assert self.process.stdout is not None
        for line in self.process.stdout:
            self.lines.put(line.rstrip("\r\n"))
        self.lines.put(None)

    def send(self, command: str) -> None:
        if self.process.poll() is not None:
            raise RuntimeError(f"engine exited with code {self.process.returncode}")
        assert self.process.stdin is not None
        self.process.stdin.write(command + "\n")
        self.process.stdin.flush()

    def wait_for(self, prefix: str, timeout: float) -> list[str]:
        deadline = time.monotonic() + timeout
        observed: list[str] = []
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError(f"engine did not emit {prefix!r}; output={observed[-5:]}")
            try:
                line = self.lines.get(timeout=remaining)
            except queue.Empty as error:
                raise TimeoutError(f"engine did not emit {prefix!r}") from error
            if line is None:
                raise RuntimeError("engine exited while a response was pending")
            observed.append(line)
            if line.startswith(prefix):
                return observed

    def measure(self, fen: str, control: TimeControl) -> tuple[float, str, list[str]]:
        board = chess.Board(fen)
        self.send("ucinewgame")
        self.send(f"position fen {fen}")
        started = time.perf_counter_ns()
        self.send(uci_go_command(control, board.turn == chess.WHITE))
        available_ms = control.base_ms if control.base_ms > 0 else control.increment_ms
        output = self.wait_for("bestmove ", available_ms / 1000 + 2.0)
        elapsed_ms = (time.perf_counter_ns() - started) / 1_000_000
        best_line = next(line for line in output if line.startswith("bestmove "))
        best_move = best_line.split()[1]
        try:
            move = chess.Move.from_uci(best_move)
        except ValueError as error:
            raise RuntimeError(f"engine returned malformed move {best_move}") from error
        if move not in board.legal_moves:
            raise RuntimeError(f"engine returned illegal move {best_move} for {fen}")
        return elapsed_ms, best_move, output

    def close(self) -> None:
        if self.process.poll() is None:
            try:
                self.send("quit")
                self.process.wait(timeout=3)
            except (OSError, RuntimeError, subprocess.TimeoutExpired):
                self.process.kill()
                self.process.wait()

    def __enter__(self) -> "PersistentUciEngine":
        return self

    def __exit__(self, *_: object) -> None:
        self.close()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--engine", type=Path, required=True)
    parser.add_argument(
        "--controls",
        default="0+1,0.5+0,1+0,1+1,2+0,0+2,2+1",
        help="comma-separated base+increment controls in seconds",
    )
    parser.add_argument("--positions", type=int, default=100)
    parser.add_argument("--repetitions", type=int, default=1)
    parser.add_argument("--seed", type=int, default=20260717)
    parser.add_argument("--threads", type=int, default=1)
    parser.add_argument("--hash", type=int, default=64)
    parser.add_argument("--move-overhead", type=int, default=30)
    args = parser.parse_args()
    if args.positions < 1 or args.repetitions < 1:
        parser.error("positions and repetitions must be positive")
    controls = [TimeControl.parse(item.strip()) for item in args.controls.split(",")]
    fens = positions(args.positions, args.seed)
    engine_path = args.engine.resolve()
    metadata = {
        "engine": str(engine_path),
        "engine_sha256": sha256(engine_path),
        "controls": [str(control) for control in controls],
        "positions": args.positions,
        "repetitions": args.repetitions,
        "seed": args.seed,
        "threads": args.threads,
        "hash_mb": args.hash,
        "move_overhead_ms": args.move_overhead,
        "persistent_process": True,
    }
    print("bullet_latency_metadata=" + json.dumps(metadata, sort_keys=True))

    with PersistentUciEngine(
        engine_path,
        threads=args.threads,
        hash_mb=args.hash,
        move_overhead_ms=args.move_overhead,
    ) as engine:
        for control in controls:
            samples: list[float] = []
            misses = 0
            deadline = control.base_ms if control.base_ms > 0 else control.increment_ms
            for _ in range(args.repetitions):
                for fen in fens:
                    elapsed, _, _ = engine.measure(fen, control)
                    samples.append(elapsed)
                    misses += elapsed >= deadline
            summary = {
                "control": str(control),
                "samples": len(samples),
                "p50_ms": percentile(samples, 0.50),
                "p95_ms": percentile(samples, 0.95),
                "p99_ms": percentile(samples, 0.99),
                "max_ms": max(samples),
                "deadline_ms": deadline,
                "deadline_misses": misses,
            }
            print("bullet_latency_summary=" + json.dumps(summary, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
