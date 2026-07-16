#!/usr/bin/env python3
"""Emit a reproducible manifest for a chess-engine match or benchmark."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import platform
import subprocess
from datetime import datetime, timezone
from pathlib import Path


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def git_revision() -> str:
    try:
        return subprocess.check_output(
            ["git", "rev-parse", "HEAD"], text=True, stderr=subprocess.DEVNULL
        ).strip()
    except (OSError, subprocess.CalledProcessError):
        return "unknown"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--engine", type=Path, required=True)
    parser.add_argument("--opponent", type=Path)
    parser.add_argument("--network", type=Path)
    parser.add_argument("--book", type=Path)
    parser.add_argument("--tc", default="10+0.1")
    parser.add_argument("--threads", type=int, default=1)
    parser.add_argument("--games", type=int, default=0)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if not args.engine.is_file():
        parser.error("engine does not exist")

    def asset(path: Path | None) -> dict[str, str] | None:
        if path is None:
            return None
        if not path.is_file():
            raise SystemExit(f"asset does not exist: {path}")
        return {"path": str(path.resolve()), "sha256": sha256(path)}

    manifest = {
        "created_utc": datetime.now(timezone.utc).isoformat(),
        "git_revision": git_revision(),
        "engine": asset(args.engine),
        "opponent": asset(args.opponent),
        "network": asset(args.network),
        "book": asset(args.book),
        "time_control": args.tc,
        "threads": args.threads,
        "games": args.games,
        "hardware": {
            "system": platform.platform(),
            "processor": platform.processor(),
            "python": platform.python_version(),
            "cpu_count": os.cpu_count(),
        },
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(manifest, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
