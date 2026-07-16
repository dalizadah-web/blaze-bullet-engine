#!/usr/bin/env python3
"""Repeated clean-process UCI startup/teardown gate."""

from __future__ import annotations

import argparse
import subprocess
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--engine", type=Path, required=True)
    parser.add_argument("--launches", type=int, default=10)
    args = parser.parse_args()
    if not args.engine.is_file() or args.launches <= 0:
        parser.error("engine must exist and launches must be positive")

    transcript = "uci\nisready\nposition startpos\ngo nodes 64\nstop\nquit\n"
    for launch in range(1, args.launches + 1):
        try:
            result = subprocess.run(
                [str(args.engine)],
                input=transcript,
                text=True,
                encoding="utf-8",
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                timeout=10,
                check=False,
            )
        except subprocess.TimeoutExpired as error:
            raise RuntimeError(f"launch {launch} timed out") from error
        if result.returncode != 0:
            raise RuntimeError(f"launch {launch} exited with {result.returncode}: {result.stdout}")
        for marker in ("uciok", "readyok", "bestmove "):
            if marker not in result.stdout:
                raise RuntimeError(f"launch {launch} omitted {marker!r}: {result.stdout}")

    print(f"{args.launches}/{args.launches} clean UCI launches passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
