#!/usr/bin/env python3
"""Create deterministic fixed-node UCI search signatures."""

from __future__ import annotations

import argparse
import hashlib
import json
import queue
import subprocess
import threading
import time
from collections.abc import Sequence
from dataclasses import dataclass
from pathlib import Path
from typing import TextIO

import chess


_INTEGER_FIELDS = {"depth", "seldepth", "nodes", "time", "nps", "hashfull"}


def _parse_integer(tokens: list[str], index: int, field: str) -> tuple[int, int]:
    if index >= len(tokens):
        raise ValueError(f"missing {field} value")
    try:
        return int(tokens[index]), index + 1
    except ValueError as exc:
        raise ValueError(f"invalid {field} value: {tokens[index]!r}") from exc


def parse_info(line: str) -> dict[str, object]:
    """Parse deterministic fields from one UCI ``info`` line strictly."""

    tokens = line.strip().split()
    if not tokens or tokens[0] != "info":
        raise ValueError("expected UCI info line")

    parsed: dict[str, object] = {}
    index = 1
    while index < len(tokens):
        field = tokens[index]
        index += 1
        if field in _INTEGER_FIELDS:
            parsed[field], index = _parse_integer(tokens, index, field)
        elif field == "score":
            if index >= len(tokens) or tokens[index] not in {"cp", "mate"}:
                raise ValueError("score must specify cp or mate")
            kind = tokens[index]
            value, index = _parse_integer(tokens, index + 1, "score")
            score: dict[str, object] = {"kind": kind, "value": value}
            bound: str | None = None
            while index < len(tokens) and tokens[index] in {"lowerbound", "upperbound"}:
                next_bound = "lower" if tokens[index] == "lowerbound" else "upper"
                if bound is not None:
                    raise ValueError("score must not specify more than one bound")
                bound = next_bound
                index += 1
            if bound is not None:
                score["bound"] = bound
            parsed["score"] = score
        elif field == "pv":
            parsed["pv"] = tokens[index:]
            break
        elif field in {"currmove", "refutation", "currline", "string"}:
            # These fields are non-canonical diagnostics. Their payload may contain
            # arbitrary tokens, so no later deterministic fields can be recovered.
            break
        else:
            # UCI permits additional fields. Ignore unknown single tokens while
            # retaining the deterministic fields understood above.
            continue
    return parsed


def canonical_signature(payload: dict[str, object]) -> str:
    """Hash a canonical JSON representation of deterministic search data."""

    encoded = json.dumps(
        payload, sort_keys=True, separators=(",", ":"), ensure_ascii=True
    ).encode("utf-8")
    return hashlib.sha256(encoded).hexdigest()


@dataclass(frozen=True)
class CorpusPosition:
    """One stable-ID position from an EPD search corpus."""

    identifier: str
    fen: str

    @property
    def board(self) -> chess.Board:
        return chess.Board(self.fen)


def load_corpus(path: str | Path) -> list[CorpusPosition]:
    """Load legal, uniquely identified EPD positions."""

    corpus_path = Path(path)
    positions: list[CorpusPosition] = []
    identifiers: set[str] = set()
    for line_number, raw_line in enumerate(
        corpus_path.read_text(encoding="utf-8").splitlines(), start=1
    ):
        line = raw_line.strip()
        if not line or line.startswith("#"):
            continue
        try:
            board, operations = chess.Board.from_epd(line)
        except (ValueError, TypeError) as exc:
            raise ValueError(f"invalid EPD on line {line_number}: {exc}") from exc
        identifier = operations.get("id")
        if not isinstance(identifier, str) or not identifier.strip():
            raise ValueError(f"EPD line {line_number} lacks a non-empty id")
        if identifier in identifiers:
            raise ValueError(f"duplicate EPD id {identifier!r} on line {line_number}")
        if not board.is_valid():
            raise ValueError(f"EPD line {line_number} is not a legal position")
        identifiers.add(identifier)
        positions.append(CorpusPosition(identifier=identifier, fen=board.fen()))
    if not positions:
        raise ValueError("search corpus is empty")
    return positions


def signature_payload(report: dict[str, object]) -> dict[str, object]:
    """Select only deterministic chess/search fields from a complete report."""

    deterministic_positions: list[dict[str, object]] = []
    for raw_position in report.get("positions", []):
        if not isinstance(raw_position, dict):
            raise ValueError("report positions must be mappings")
        position = {
            key: raw_position[key]
            for key in (
                "id",
                "fen",
                "bestmove",
                "ponder",
                "depth",
                "seldepth",
                "score",
                "nodes",
                "pv",
            )
            if key in raw_position
        }
        deterministic_positions.append(position)
    aggregate = report.get("aggregate", {})
    deterministic_aggregate = (
        {"nodes": aggregate["nodes"]}
        if isinstance(aggregate, dict) and "nodes" in aggregate
        else {}
    )
    return {
        key: report[key]
        for key in (
            "schema_version",
            "engine_sha256",
            "corpus_sha256",
            "uci",
            "settings",
        )
        if key in report
    } | {
        "positions": deterministic_positions,
        "aggregate": deterministic_aggregate,
    }


class UciEngine:
    """One persistent, timeout-bounded UCI engine process."""

    def __init__(
        self,
        engine: str | Sequence[str],
        *,
        threads: int = 1,
        hash_mb: int = 64,
        timeout: float = 10.0,
        shutdown_timeout: float = 1.0,
    ) -> None:
        if threads < 1:
            raise ValueError("threads must be positive")
        if hash_mb < 1:
            raise ValueError("hash_mb must be positive")
        if timeout <= 0 or shutdown_timeout <= 0:
            raise ValueError("timeouts must be positive")

        command = [engine] if isinstance(engine, str) else list(engine)
        if not command:
            raise ValueError("engine command must not be empty")
        self.timeout = timeout
        self.shutdown_timeout = shutdown_timeout
        self.identity: dict[str, str] = {}
        self.options: list[str] = []
        self._closed = False
        self._lines: queue.Queue[str | None] = queue.Queue()
        self._process = subprocess.Popen(
            command,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            text=True,
            encoding="utf-8",
            errors="strict",
            bufsize=1,
        )
        assert self._process.stdout is not None
        self._reader = threading.Thread(
            target=self._read_stdout,
            args=(self._process.stdout,),
            name="uci-signature-reader",
            daemon=True,
        )
        self._reader.start()
        try:
            self._handshake(threads, hash_mb)
        except BaseException:
            self.close()
            raise

    @property
    def returncode(self) -> int | None:
        return self._process.poll()

    @property
    def reader_alive(self) -> bool:
        return self._reader.is_alive()

    def _read_stdout(self, stdout: TextIO) -> None:
        try:
            for line in stdout:
                self._lines.put(line.rstrip("\r\n"))
        except (OSError, ValueError):
            # close() may close the pipe while the daemon reader is unwinding.
            pass
        finally:
            self._lines.put(None)

    def _send(self, command: str) -> None:
        if self._closed or self._process.poll() is not None:
            raise RuntimeError("UCI engine is not running")
        assert self._process.stdin is not None
        self._process.stdin.write(command + "\n")
        self._process.stdin.flush()

    def _read_line(self, deadline: float, expected: str) -> str:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise TimeoutError(f"timed out waiting for {expected}")
        try:
            line = self._lines.get(timeout=remaining)
        except queue.Empty as exc:
            raise TimeoutError(f"timed out waiting for {expected}") from exc
        if line is None:
            raise RuntimeError(f"UCI engine exited while waiting for {expected}")
        return line

    def _wait_for(self, expected: str) -> list[str]:
        deadline = time.monotonic() + self.timeout
        received: list[str] = []
        while True:
            line = self._read_line(deadline, expected)
            received.append(line)
            if line == expected:
                return received

    def _handshake(self, threads: int, hash_mb: int) -> None:
        self._send("uci")
        for line in self._wait_for("uciok"):
            if line.startswith("id name "):
                self.identity["name"] = line[len("id name ") :]
            elif line.startswith("id author "):
                self.identity["author"] = line[len("id author ") :]
            elif line.startswith("option "):
                self.options.append(line)
        if "name" not in self.identity:
            raise ValueError("UCI engine did not advertise an id name")
        advertised_names: set[str] = set()
        for option in self.options:
            tokens = option.split()
            try:
                name_start = tokens.index("name") + 1
                type_start = tokens.index("type", name_start)
            except ValueError:
                continue
            advertised_names.add(" ".join(tokens[name_start:type_start]).casefold())
        missing = [
            name for name in ("Hash", "Threads") if name.casefold() not in advertised_names
        ]
        if missing:
            raise ValueError(
                "UCI engine did not advertise required options: " + ", ".join(missing)
            )
        self._send(f"setoption name Threads value {threads}")
        self._send(f"setoption name Hash value {hash_mb}")
        self._send("isready")
        self._wait_for("readyok")

    def search(self, fen: str, nodes: int) -> dict[str, object]:
        """Search one legal non-terminal FEN and return final UCI result fields."""

        if nodes < 1:
            raise ValueError("nodes must be positive")
        try:
            board = chess.Board(fen)
        except ValueError as exc:
            raise ValueError(f"invalid FEN: {fen!r}") from exc
        if not board.is_valid():
            raise ValueError(f"invalid chess position: {fen!r}")
        if board.is_game_over(claim_draw=False):
            raise ValueError("signature positions must have at least one legal move")

        self._send("ucinewgame")
        self._send("isready")
        self._wait_for("readyok")
        self._send(f"position fen {fen}")
        self._send(f"go nodes {nodes}")

        deadline = time.monotonic() + self.timeout
        final_info: dict[str, object] | None = None
        try:
            while True:
                line = self._read_line(deadline, "bestmove")
                if line.startswith("info "):
                    parsed = parse_info(line)
                    required = {"depth", "score", "nodes", "time", "pv"}
                    if required.issubset(parsed):
                        final_info = parsed
                    continue
                if not line.startswith("bestmove "):
                    continue
                fields = line.split()
                if len(fields) not in {2, 4} or (len(fields) == 4 and fields[2] != "ponder"):
                    raise ValueError(f"malformed bestmove line: {line!r}")
                if final_info is None:
                    raise ValueError("bestmove received without complete info")
                bestmove = fields[1]
                try:
                    move = chess.Move.from_uci(bestmove)
                except ValueError as exc:
                    raise ValueError(f"invalid bestmove: {bestmove!r}") from exc
                if move not in board.legal_moves:
                    raise ValueError(f"illegal bestmove {bestmove!r} for {fen!r}")
                pv = final_info["pv"]
                if not isinstance(pv, list) or not pv:
                    raise ValueError("complete info must contain a non-empty PV")
                pv_board = board.copy(stack=False)
                for ply, token in enumerate(pv, start=1):
                    if not isinstance(token, str):
                        raise ValueError(f"invalid PV move at ply {ply}: {token!r}")
                    try:
                        pv_move = chess.Move.from_uci(token)
                    except ValueError as exc:
                        raise ValueError(
                            f"invalid PV move at ply {ply}: {token!r}"
                        ) from exc
                    if pv_move not in pv_board.legal_moves:
                        raise ValueError(
                            f"illegal PV move at ply {ply}: {token!r}"
                        )
                    pv_board.push(pv_move)
                if pv[0] != bestmove:
                    raise ValueError(
                        f"PV first move {pv[0]!r} does not match bestmove {bestmove!r}"
                    )
                result: dict[str, object] = {
                    "bestmove": bestmove,
                    "info": final_info,
                }
                if len(fields) == 4:
                    ponder = fields[3]
                    child = board.copy(stack=False)
                    child.push(move)
                    try:
                        ponder_move = chess.Move.from_uci(ponder)
                    except ValueError as exc:
                        raise ValueError(f"invalid ponder move: {ponder!r}") from exc
                    if ponder_move not in child.legal_moves:
                        raise ValueError(
                            f"illegal ponder {ponder!r} after {bestmove!r}"
                        )
                    result["ponder"] = ponder
                return result
        except (TimeoutError, RuntimeError, ValueError):
            # Any malformed result leaves unread protocol output in the pipe. The
            # process is poisoned and cannot safely be reused for another search.
            self.close()
            raise

    def close(self) -> None:
        """Request clean shutdown and kill a non-cooperative process."""

        if self._closed:
            return
        self._closed = True
        if self._process.poll() is None:
            try:
                assert self._process.stdin is not None
                self._process.stdin.write("quit\n")
                self._process.stdin.flush()
            except (BrokenPipeError, OSError):
                pass
            try:
                self._process.wait(timeout=self.shutdown_timeout)
            except subprocess.TimeoutExpired:
                self._process.kill()
                self._process.wait(timeout=self.shutdown_timeout)
        if self._process.stdin is not None:
            self._process.stdin.close()
        if self._process.stdout is not None:
            self._process.stdout.close()
        self._reader.join(timeout=self.shutdown_timeout)
        if self._reader.is_alive():
            raise RuntimeError("UCI reader thread did not terminate during close")

    def __enter__(self) -> "UciEngine":
        return self

    def __exit__(self, *_: object) -> None:
        self.close()


def _sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def generate_report(
    engine: str | Sequence[str],
    corpus: str | Path,
    *,
    nodes: int,
    threads: int,
    hash_mb: int,
    timeout: float = 30.0,
) -> dict[str, object]:
    """Run a corpus through one persistent engine and build a signed report."""

    command = [engine] if isinstance(engine, str) else list(engine)
    if not command:
        raise ValueError("engine command must not be empty")
    engine_path = Path(command[0])
    corpus_path = Path(corpus)
    positions = load_corpus(corpus_path)
    report_positions: list[dict[str, object]] = []

    with UciEngine(
        command,
        threads=threads,
        hash_mb=hash_mb,
        timeout=timeout,
    ) as uci:
        for position in positions:
            result = uci.search(position.fen, nodes)
            info = result["info"]
            assert isinstance(info, dict)
            measured_time = int(info["time"])
            measured_nodes = int(info["nodes"])
            entry: dict[str, object] = {
                "id": position.identifier,
                "fen": position.fen,
                "bestmove": result["bestmove"],
                "depth": info["depth"],
                "score": info["score"],
                "nodes": measured_nodes,
                "pv": info["pv"],
                "time_ms": measured_time,
                "nps": info.get(
                    "nps", measured_nodes * 1000 // max(measured_time, 1)
                ),
            }
            if "seldepth" in info:
                entry["seldepth"] = info["seldepth"]
            if "ponder" in result:
                entry["ponder"] = result["ponder"]
            report_positions.append(entry)

        uci_identity = dict(uci.identity)
        uci_options = list(uci.options)

    total_nodes = sum(int(position["nodes"]) for position in report_positions)
    total_time = sum(int(position["time_ms"]) for position in report_positions)
    report: dict[str, object] = {
        "schema_version": 1,
        "engine_sha256": _sha256_file(engine_path),
        "corpus_sha256": _sha256_file(corpus_path),
        "uci": {"identity": uci_identity, "options": uci_options},
        "settings": {"nodes": nodes, "threads": threads, "hash_mb": hash_mb},
        "positions": report_positions,
        "aggregate": {
            "nodes": total_nodes,
            "time_ms": total_time,
            "nps": total_nodes * 1000 // max(total_time, 1),
        },
    }
    report["signature"] = canonical_signature(signature_payload(report))
    return report


def _parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Create a deterministic fixed-node Blaze search signature"
    )
    parser.add_argument("--engine", required=True, type=Path)
    parser.add_argument("--corpus", required=True, type=Path)
    parser.add_argument("--nodes", required=True, type=int)
    parser.add_argument("--threads", required=True, type=int)
    parser.add_argument("--hash-mb", required=True, type=int)
    parser.add_argument("--output", required=True, type=Path)
    return parser.parse_args()


def main() -> int:
    arguments = _parse_arguments()
    report = generate_report(
        str(arguments.engine),
        arguments.corpus,
        nodes=arguments.nodes,
        threads=arguments.threads,
        hash_mb=arguments.hash_mb,
    )
    arguments.output.parent.mkdir(parents=True, exist_ok=True)
    arguments.output.write_text(
        json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    print(report["signature"])
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
