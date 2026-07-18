"""Select a reproducible, side-balanced confirmation suite from a large EPD."""

from __future__ import annotations

import argparse
import hashlib
import heapq
import json
from pathlib import Path
from typing import BinaryIO


ALGORITHM_VERSION = "sha256-priority-side-balanced-v1"


def _selection_digest(tag: bytes, seed: bytes, line_number: int, line: bytes) -> bytes:
    return hashlib.sha256(
        tag
        + b"\0"
        + seed
        + b"\0"
        + str(line_number).encode("ascii")
        + b"\0"
        + line
    ).digest()


def _offer(
    heap: list[tuple[int, int, bytes]],
    active: dict[bytes, tuple[bytes, int]],
    *,
    digest: bytes,
    line_number: int,
    line: bytes,
    quota: int,
) -> None:
    priority = (digest, line_number, line)
    previous = active.get(line)
    if previous is not None and priority >= (previous[0], previous[1], line):
        return
    # Python's heap is a min-heap. Negating the digest and line number keeps
    # the worst retained priority at the root. Stale duplicate entries are
    # lazily removed, while the active map stays bounded by the quota.
    item = (-int.from_bytes(digest, "big"), -line_number, line)
    if previous is not None:
        active[line] = (digest, line_number)
        heapq.heappush(heap, item)
        return
    while heap:
        negative_digest, negative_line, worst_line = heap[0]
        current = active.get(worst_line)
        if current == (
            (-negative_digest).to_bytes(32, "big"),
            -negative_line,
        ):
            break
        heapq.heappop(heap)
    if len(active) < quota:
        active[line] = (digest, line_number)
        heapq.heappush(heap, item)
    elif item > heap[0]:
        _, _, worst_line = heapq.heappop(heap)
        del active[worst_line]
        active[line] = (digest, line_number)
        heapq.heappush(heap, item)
    if len(heap) > quota * 4:
        heap[:] = [
            (-int.from_bytes(value[0], "big"), -value[1], selected_line)
            for selected_line, value in active.items()
        ]
        heapq.heapify(heap)


def _stream_candidates(
    stream: BinaryIO, *, seed: bytes, quota: int
) -> tuple[dict[bytes, dict[bytes, tuple[bytes, int]]], str, dict[bytes, int], int]:
    heaps: dict[bytes, list[tuple[int, int, bytes]]] = {b"w": [], b"b": []}
    active: dict[bytes, dict[bytes, tuple[bytes, int]]] = {b"w": {}, b"b": {}}
    source_hash = hashlib.sha256()
    side_counts = {b"w": 0, b"b": 0}
    nonempty = 0
    for line_number, raw in enumerate(stream, 1):
        source_hash.update(raw)
        line = raw.strip()
        if not line:
            continue
        nonempty += 1
        fields = line.split()
        if len(fields) < 2 or fields[1] not in heaps:
            raise ValueError(f"invalid EPD side-to-move field on source line {line_number}")
        side = fields[1]
        side_counts[side] += 1
        _offer(
            heaps[side],
            active[side],
            digest=_selection_digest(b"select", seed, line_number, line),
            line_number=line_number,
            line=line,
            quota=quota,
        )
    return active, source_hash.hexdigest(), side_counts, nonempty


def select_openings(
    source: Path | str,
    output: Path | str,
    *,
    seed: int,
    quota_per_side: int,
    expected_source_sha256: str | None = None,
    expected_source_nonempty_lines: int | None = None,
) -> dict[str, object]:
    if seed < 0:
        raise ValueError("seed must be nonnegative")
    if quota_per_side <= 0:
        raise ValueError("quota_per_side must be positive")
    seed_bytes = str(seed).encode("ascii")
    source_path = Path(source)
    with source_path.open("rb") as stream:
        selected_by_side, source_sha256, side_counts, nonempty = _stream_candidates(
            stream, seed=seed_bytes, quota=quota_per_side
        )
    if expected_source_sha256 is not None and source_sha256 != expected_source_sha256.lower():
        raise ValueError(
            f"source SHA-256 mismatch: expected {expected_source_sha256.lower()}, got {source_sha256}"
        )
    if expected_source_nonempty_lines is not None and nonempty != expected_source_nonempty_lines:
        raise ValueError(
            "source nonempty line count mismatch: "
            f"expected {expected_source_nonempty_lines}, got {nonempty}"
        )
    for side in (b"w", b"b"):
        if len(selected_by_side[side]) != quota_per_side:
            raise ValueError(
                f"source has only {side_counts[side]} {side.decode()} positions; "
                f"need {quota_per_side}"
            )

    ordered: dict[bytes, list[tuple[bytes, int, bytes]]] = {}
    for side in (b"w", b"b"):
        selected = [
            (selection[1], line)
            for line, selection in selected_by_side[side].items()
        ]
        ordered[side] = sorted(
            (
                _selection_digest(b"order", seed_bytes, line_number, line),
                line_number,
                line,
            )
            for line_number, line in selected
        )
    interleaved = [
        ordered[side][index]
        for index in range(quota_per_side)
        for side in (b"w", b"b")
    ]
    lines = [entry[2] for entry in interleaved]
    if len(set(lines)) != len(lines):
        raise ValueError("selected opening lines are not unique")
    encoded = b"".join(line + b"\n" for line in lines)
    destination = Path(output)
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_bytes(encoded)
    source_line_numbers = [entry[1] for entry in interleaved]
    line_number_bytes = b"".join(
        str(line_number).encode("ascii") + b"\n" for line_number in source_line_numbers
    )
    return {
        "algorithm_version": ALGORITHM_VERSION,
        "seed": seed,
        "quota_per_side": quota_per_side,
        "source_sha256": source_sha256,
        "source_nonempty_lines": nonempty,
        "source_side_counts": {
            "white": side_counts[b"w"],
            "black": side_counts[b"b"],
        },
        "output_sha256": hashlib.sha256(encoded).hexdigest(),
        "ordered_source_line_sha256": hashlib.sha256(line_number_bytes).hexdigest(),
        "selected_source_lines": source_line_numbers,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--seed", type=int, required=True)
    parser.add_argument("--quota-per-side", type=int, required=True)
    parser.add_argument("--expected-source-sha256")
    parser.add_argument("--expected-source-nonempty-lines", type=int)
    parser.add_argument("--metadata", type=Path)
    args = parser.parse_args()
    metadata = select_openings(
        args.source,
        args.output,
        seed=args.seed,
        quota_per_side=args.quota_per_side,
        expected_source_sha256=args.expected_source_sha256,
        expected_source_nonempty_lines=args.expected_source_nonempty_lines,
    )
    encoded = json.dumps(metadata, indent=2, sort_keys=True) + "\n"
    if args.metadata:
        args.metadata.parent.mkdir(parents=True, exist_ok=True)
        args.metadata.write_text(encoded, encoding="utf-8", newline="\n")
    print(encoded, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
