#!/usr/bin/env python3
"""Run a comparable UCI benchmark on deterministic legal positions."""

from __future__ import annotations

import argparse
import hashlib
import json
import platform
import queue
import random
import statistics
import subprocess
import time
from pathlib import Path

import chess


DEFAULT_COMPILER_FLAGS = "-std=c++20 -O3 -DNDEBUG -Wall -Wextra -Wpedantic -Werror -Isrc"


def summarize_rates(rates: list[float]) -> dict[str, float]:
    if not rates:
        raise ValueError("benchmark requires at least one NPS sample")
    if len(rates) == 1:
        q1 = q3 = rates[0]
    else:
        q1, _, q3 = statistics.quantiles(rates, n=4, method="inclusive")
    return {
        "median_nps": statistics.median(rates),
        "q1_nps": q1,
        "q3_nps": q3,
        "iqr_nps": q3 - q1,
    }


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def cpu_name() -> str:
    if platform.system() == "Windows":
        try:
            import winreg
            with winreg.OpenKey(
                winreg.HKEY_LOCAL_MACHINE,
                r"HARDWARE\DESCRIPTION\System\CentralProcessor\0",
            ) as key:
                return str(winreg.QueryValueEx(key, "ProcessorNameString")[0]).strip()
        except OSError:
            pass
    return platform.processor() or platform.machine()


def compiler_identity() -> str:
    try:
        completed = subprocess.run(
            ["g++", "--version"],
            check=True,
            capture_output=True,
            text=True,
            timeout=5,
        )
        return completed.stdout.splitlines()[0]
    except (OSError, subprocess.SubprocessError, IndexError):
        return "unknown"


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
    parser.add_argument("--repetitions", type=int, default=3)
    parser.add_argument("--compiler-flags", default=DEFAULT_COMPILER_FLAGS)
    args = parser.parse_args()
    if args.positions < 1 or args.repetitions < 1:
        parser.error("positions and repetitions must be positive")
    fens = positions(args.positions, args.seed)
    for engine in args.engine:
        engine = engine.resolve()
        metadata = {
            "engine": str(engine),
            "engine_sha256": sha256(engine),
            "cpu": cpu_name(),
            "compiler": compiler_identity(),
            "compiler_flags": args.compiler_flags,
            "positions": args.positions,
            "repetitions": args.repetitions,
            "milliseconds": args.milliseconds,
            "depth": args.depth,
            "threads": args.threads,
            "seed": args.seed,
        }
        print("benchmark_metadata=" + json.dumps(metadata, sort_keys=True))
        total_nodes = 0
        rates: list[float] = []
        for repetition in range(1, args.repetitions + 1):
            for index, fen in enumerate(fens, start=1):
                depth, nodes, best, elapsed = run_one(
                    engine, fen, args.milliseconds, args.threads, args.depth, args.nnue)
                if chess.Move.from_uci(best) not in chess.Board(fen).legal_moves:
                    raise RuntimeError(
                        f"{engine} returned illegal move {best} on position {index}")
                total_nodes += nodes
                nps = nodes * 1000.0 / max(elapsed, 1)
                rates.append(nps)
                print(
                    f"  repetition={repetition:02d} position={index:02d} depth={depth:2d} "
                    f"nodes={nodes:9d} elapsed_ms={elapsed:4d} nps={nps:10.1f} best={best}")
        summary = summarize_rates(rates)
        summary.update({
            "samples": len(rates),
            "total_nodes": total_nodes,
            "average_nodes": total_nodes / len(rates),
        })
        print("benchmark_summary=" + json.dumps(summary, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
