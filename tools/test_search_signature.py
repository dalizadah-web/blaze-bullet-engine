"""Tests for deterministic fixed-node search signatures."""

from __future__ import annotations

import hashlib
import json
from pathlib import Path
import sys
import tempfile
import unittest

from tools.search_signature import (
    UciEngine,
    canonical_signature,
    generate_report,
    load_corpus,
    parse_info,
    signature_payload,
)


_FAKE_ENGINE = r'''#!/usr/bin/env python3
import os
import sys
import time

mode = sys.argv[1]
log_path = sys.argv[2]
ready_count = 0

def emit(line):
    print(line, flush=True)

for raw in sys.stdin:
    command = raw.strip()
    with open(log_path, "a", encoding="utf-8") as log:
        log.write(command + "\n")
    if command == "uci":
        emit("id name Deterministic Fake")
        emit("id author Blaze Tests")
        if mode != "no-options":
            emit("option name Hash type spin default 16 min 1 max 4096")
            emit("option name Threads type spin default 1 min 1 max 128")
        emit("uciok")
    elif command == "isready":
        ready_count += 1
        if mode == "stall-ready" and ready_count > 1:
            time.sleep(5)
        else:
            emit("readyok")
    elif command.startswith("go nodes "):
        if mode == "timeout":
            time.sleep(5)
        elif mode == "malformed":
            emit("info depth 4 score cp nodes 100")
            emit("bestmove e2e4")
        elif mode == "incomplete":
            emit("info depth 4 score cp 12 nodes 100 time 3")
            emit("bestmove e2e4")
        elif mode == "illegal":
            emit("info depth 4 seldepth 6 score cp 12 nodes 100 time 3 pv e2e4")
            emit("bestmove a1a8")
        elif mode == "empty-pv":
            emit("info depth 4 seldepth 6 score cp 12 nodes 100 time 3 pv")
            emit("bestmove e2e4")
        elif mode == "invalid-pv":
            emit("info depth 4 seldepth 6 score cp 12 nodes 100 time 3 pv h9h8")
            emit("bestmove e2e4")
        elif mode == "mismatched-pv":
            emit("info depth 4 seldepth 6 score cp 12 nodes 100 time 3 pv d2d4")
            emit("bestmove e2e4")
        elif mode == "truncated-pv":
            emit("info depth 4 seldepth 6 score cp 12 nodes 100 time 3 pv e2e4 e7e5 e2e3")
            emit("bestmove e2e4")
        elif mode == "illegal-ponder":
            emit("info depth 4 seldepth 6 score cp 12 nodes 100 time 3 pv e2e4 e7e5")
            emit("bestmove e2e4 ponder e2e5")
        else:
            emit("info depth 1 seldepth 1 score cp 2 nodes 10 time 1 pv d2d4")
            emit("info depth 4 seldepth 6 score cp 12 nodes 100 time 3 nps 33333 pv e2e4 e7e5")
            emit("bestmove e2e4 ponder e7e5")
    elif command == "quit":
        if mode != "ignore-quit":
            break
'''


class SearchSignatureTests(unittest.TestCase):
    def test_parse_info_extracts_score_nodes_and_pv(self) -> None:
        parsed = parse_info(
            "info depth 7 seldepth 11 score cp 34 nodes 1000 time 5 "
            "pv e2e4 e7e5"
        )

        self.assertEqual(parsed["depth"], 7)
        self.assertEqual(parsed["seldepth"], 11)
        self.assertEqual(parsed["score"], {"kind": "cp", "value": 34})
        self.assertEqual(parsed["nodes"], 1000)
        self.assertEqual(parsed["time"], 5)
        self.assertEqual(parsed["pv"], ["e2e4", "e7e5"])

    def test_parse_info_accepts_mate_scores(self) -> None:
        parsed = parse_info("info depth 8 score mate -3 nodes 200 pv e1e2")

        self.assertEqual(parsed["score"], {"kind": "mate", "value": -3})

    def test_parse_info_preserves_score_bound_semantics(self) -> None:
        exact = parse_info("info depth 4 score cp 12 nodes 100")
        lower = parse_info("info depth 4 score cp 12 lowerbound nodes 100")
        upper = parse_info("info depth 4 score cp 12 upperbound nodes 100")

        self.assertEqual(exact["score"], {"kind": "cp", "value": 12})
        self.assertEqual(
            lower["score"], {"kind": "cp", "value": 12, "bound": "lower"}
        )
        self.assertEqual(
            upper["score"], {"kind": "cp", "value": 12, "bound": "upper"}
        )
        self.assertNotEqual(
            canonical_signature({"score": exact["score"]}),
            canonical_signature({"score": lower["score"]}),
        )

    def test_parse_info_rejects_conflicting_score_bounds(self) -> None:
        with self.assertRaisesRegex(ValueError, "bound"):
            parse_info(
                "info depth 4 score cp 12 lowerbound upperbound nodes 100"
            )

    def test_parse_info_rejects_incomplete_score(self) -> None:
        with self.assertRaisesRegex(ValueError, "score"):
            parse_info("info depth 4 score cp nodes 100")

    def test_parse_info_rejects_non_info_lines(self) -> None:
        with self.assertRaisesRegex(ValueError, "info"):
            parse_info("bestmove e2e4")

    def test_signature_changes_when_engine_identity_changes(self) -> None:
        first = canonical_signature(
            {"engine_sha256": "a" * 64, "positions": []}
        )
        second = canonical_signature(
            {"engine_sha256": "b" * 64, "positions": []}
        )

        self.assertNotEqual(first, second)

    def test_signature_is_independent_of_mapping_insertion_order(self) -> None:
        first = canonical_signature({"settings": {"nodes": 1000}, "positions": []})
        second = canonical_signature({"positions": [], "settings": {"nodes": 1000}})

        self.assertEqual(first, second)

    def test_signature_payload_excludes_wall_clock_fields(self) -> None:
        report = {
            "engine_sha256": "a" * 64,
            "corpus_sha256": "b" * 64,
            "uci": {"identity": {"name": "Blaze"}, "options": []},
            "settings": {"nodes": 1000, "threads": 1, "hash_mb": 16},
            "positions": [
                {
                    "id": "one",
                    "fen": "8/8/8/8/8/5k2/8/7K w - - 0 1",
                    "bestmove": "h1g1",
                    "depth": 2,
                    "score": {"kind": "cp", "value": 0},
                    "nodes": 1000,
                    "pv": ["h1g1"],
                    "time_ms": 4,
                    "nps": 250000,
                }
            ],
            "aggregate": {"nodes": 1000, "time_ms": 4, "nps": 250000},
        }
        changed = json.loads(json.dumps(report))
        changed["positions"][0]["time_ms"] = 999
        changed["positions"][0]["nps"] = 1
        changed["aggregate"]["time_ms"] = 999
        changed["aggregate"]["nps"] = 1

        self.assertEqual(
            canonical_signature(signature_payload(report)),
            canonical_signature(signature_payload(changed)),
        )

    def test_signature_payload_includes_schema_identity(self) -> None:
        first = {
            "schema_version": 1,
            "engine_sha256": "a" * 64,
            "positions": [],
        }
        second = dict(first, schema_version=2)

        self.assertNotEqual(
            canonical_signature(signature_payload(first)),
            canonical_signature(signature_payload(second)),
        )


class UciEngineTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary_directory = tempfile.TemporaryDirectory()
        directory = Path(self.temporary_directory.name)
        self.fake_engine = directory / "fake_engine.py"
        self.fake_engine.write_text(_FAKE_ENGINE, encoding="utf-8")
        self.command_log = directory / "commands.log"

    def tearDown(self) -> None:
        self.temporary_directory.cleanup()

    def command(self, mode: str = "normal") -> list[str]:
        return [sys.executable, str(self.fake_engine), mode, str(self.command_log)]

    def commands(self) -> list[str]:
        return self.command_log.read_text(encoding="utf-8").splitlines()

    def test_persistent_engine_handshake_options_and_position_lifecycle(self) -> None:
        engine = UciEngine(self.command(), threads=2, hash_mb=32, timeout=1.0)
        try:
            result = engine.search(
                "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
                100,
            )
        finally:
            engine.close()

        self.assertEqual(engine.identity["name"], "Deterministic Fake")
        self.assertEqual(engine.identity["author"], "Blaze Tests")
        self.assertEqual(len(engine.options), 2)
        self.assertEqual(result["bestmove"], "e2e4")
        self.assertEqual(result["ponder"], "e7e5")
        self.assertEqual(result["info"]["depth"], 4)
        self.assertEqual(result["info"]["nodes"], 100)
        self.assertEqual(result["info"]["pv"], ["e2e4", "e7e5"])
        commands = self.commands()
        self.assertEqual(commands[0], "uci")
        self.assertIn("setoption name Threads value 2", commands)
        self.assertIn("setoption name Hash value 32", commands)
        self.assertLess(commands.index("ucinewgame"), commands.index("position fen rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1"))
        self.assertIn("go nodes 100", commands)
        self.assertEqual(commands[-1], "quit")

    def test_two_searches_each_reset_and_synchronize_the_engine(self) -> None:
        engine = UciEngine(self.command(), timeout=1.0)
        try:
            for _ in range(2):
                engine.search(
                    "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
                    100,
                )
        finally:
            engine.close()

        commands = self.commands()
        self.assertEqual(commands.count("ucinewgame"), 2)
        self.assertEqual(commands.count("isready"), 3)
        first_reset = commands.index("ucinewgame")
        second_reset = commands.index("ucinewgame", first_reset + 1)
        for reset in (first_reset, second_reset):
            self.assertEqual(commands[reset + 1], "isready")
            self.assertTrue(commands[reset + 2].startswith("position fen "))
            self.assertEqual(commands[reset + 3], "go nodes 100")

    def test_handshake_rejects_unadvertised_deterministic_options(self) -> None:
        with self.assertRaisesRegex(ValueError, "Hash.*Threads|Threads.*Hash"):
            UciEngine(self.command("no-options"), timeout=1.0)

    def test_search_rejects_malformed_info(self) -> None:
        engine = UciEngine(self.command("malformed"), timeout=1.0)
        try:
            with self.assertRaisesRegex(ValueError, "score"):
                engine.search(
                    "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
                    100,
                )
        finally:
            engine.close()

        self.assertIsNotNone(engine.returncode)
        self.assertFalse(engine.reader_alive)

    def test_search_rejects_incomplete_final_info(self) -> None:
        engine = UciEngine(self.command("incomplete"), timeout=1.0)
        try:
            with self.assertRaisesRegex(ValueError, "complete info"):
                engine.search(
                    "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
                    100,
                )
        finally:
            engine.close()

    def test_search_rejects_illegal_bestmove(self) -> None:
        engine = UciEngine(self.command("illegal"), timeout=1.0)
        try:
            with self.assertRaisesRegex(ValueError, "illegal bestmove"):
                engine.search(
                    "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
                    100,
                )
        finally:
            engine.close()

    def test_search_rejects_empty_pv(self) -> None:
        engine = UciEngine(self.command("empty-pv"), timeout=1.0)
        with self.assertRaisesRegex(ValueError, "non-empty PV"):
            engine.search(
                "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
                100,
            )
        self.assertIsNotNone(engine.returncode)

    def test_search_rejects_invalid_pv_token(self) -> None:
        engine = UciEngine(self.command("invalid-pv"), timeout=1.0)
        with self.assertRaisesRegex(ValueError, "invalid PV move"):
            engine.search(
                "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
                100,
            )
        self.assertIsNotNone(engine.returncode)

    def test_search_rejects_pv_that_does_not_start_with_bestmove(self) -> None:
        engine = UciEngine(self.command("mismatched-pv"), timeout=1.0)
        with self.assertRaisesRegex(ValueError, "PV.*bestmove"):
            engine.search(
                "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
                100,
            )
        self.assertIsNotNone(engine.returncode)

    def test_search_rejects_sequentially_illegal_pv(self) -> None:
        engine = UciEngine(self.command("truncated-pv"), timeout=1.0)
        with self.assertRaisesRegex(ValueError, "illegal PV move"):
            engine.search(
                "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
                100,
            )
        self.assertIsNotNone(engine.returncode)

    def test_search_rejects_illegal_ponder_on_child_board(self) -> None:
        engine = UciEngine(self.command("illegal-ponder"), timeout=1.0)
        with self.assertRaisesRegex(ValueError, "illegal ponder"):
            engine.search(
                "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
                100,
            )
        self.assertIsNotNone(engine.returncode)

    def test_search_timeout_closes_process(self) -> None:
        engine = UciEngine(self.command("timeout"), timeout=0.5)
        with self.assertRaisesRegex(TimeoutError, "bestmove"):
            engine.search(
                "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
                100,
            )

        engine.close()
        self.assertIsNotNone(engine.returncode)

    def test_per_position_readiness_timeout_closes_process_and_reader(self) -> None:
        engine = UciEngine(self.command("stall-ready"), timeout=0.5)
        try:
            with self.assertRaisesRegex(TimeoutError, "readyok"):
                engine.search(
                    "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
                    100,
                )
            self.assertIsNotNone(engine.returncode)
            self.assertFalse(engine.reader_alive)
        finally:
            engine.close()

    def test_close_kills_engine_that_ignores_quit(self) -> None:
        engine = UciEngine(
            self.command("ignore-quit"), timeout=1.0, shutdown_timeout=0.05
        )

        engine.close()

        self.assertIsNotNone(engine.returncode)
        self.assertFalse(engine.reader_alive)

    def test_search_accepts_complete_info_without_seldepth(self) -> None:
        script = self.fake_engine.read_text(encoding="utf-8").replace(
            "info depth 4 seldepth 6 score cp 12 nodes 100 time 3 nps 33333",
            "info depth 4 score cp 12 nodes 100 time 3 nps 33333",
        )
        self.fake_engine.write_text(script, encoding="utf-8")
        engine = UciEngine(self.command(), timeout=1.0)
        try:
            result = engine.search(
                "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
                100,
            )
        finally:
            engine.close()

        self.assertEqual(result["info"]["depth"], 4)


class CorpusTests(unittest.TestCase):
    def test_load_corpus_preserves_rule50_counter_and_ids(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            corpus = Path(directory) / "corpus.epd"
            corpus.write_text(
                '8/8/8/8/8/5k2/8/7K w - - hmvc 97; fmvn 60; id "rule50-01";\n',
                encoding="utf-8",
            )
            positions = load_corpus(corpus)

        self.assertEqual(len(positions), 1)
        self.assertEqual(positions[0].identifier, "rule50-01")
        self.assertEqual(positions[0].fen.split()[4:], ["97", "60"])

    def test_load_corpus_rejects_duplicate_ids(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            corpus = Path(directory) / "corpus.epd"
            corpus.write_text(
                '8/8/8/8/8/5k2/8/7K w - - id "same";\n'
                '8/8/8/8/8/8/5k2/7K w - - id "same";\n',
                encoding="utf-8",
            )

            with self.assertRaisesRegex(ValueError, "duplicate.*same"):
                load_corpus(corpus)

    def test_load_corpus_rejects_illegal_position(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            corpus = Path(directory) / "corpus.epd"
            corpus.write_text(
                '8/8/8/8/8/8/8/7K w - - id "missing-black-king";\n',
                encoding="utf-8",
            )

            with self.assertRaisesRegex(ValueError, "legal"):
                load_corpus(corpus)

    def test_repository_corpus_and_provenance_are_consistent(self) -> None:
        root = Path(__file__).resolve().parents[1]
        corpus_path = root / "testdata" / "search" / "correctness-v1.epd"
        provenance_path = root / "provenance" / "search-corpora.json"

        positions = load_corpus(corpus_path)
        provenance = json.loads(provenance_path.read_text(encoding="utf-8"))
        entry = provenance["search_corpora"][0]

        self.assertGreaterEqual(len(positions), 64)
        self.assertEqual(len({position.identifier for position in positions}), len(positions))
        self.assertEqual(entry["position_count"], len(positions))
        self.assertEqual(entry["license"], "CC0-1.0")
        self.assertFalse(entry["stockfish_derived"])
        self.assertEqual(
            entry["sha256"], hashlib.sha256(corpus_path.read_bytes()).hexdigest()
        )
        prefixes = {position.identifier.split("-", 1)[0] for position in positions}
        self.assertTrue(
            {
                "opening",
                "middlegame",
                "tactical",
                "quiet",
                "checked",
                "castling",
                "enpassant",
                "promotion",
                "lowmaterial",
                "rule50",
            }.issubset(prefixes)
        )

        by_prefix = {
            prefix: [position for position in positions if position.identifier.startswith(prefix + "-")]
            for prefix in prefixes
        }
        self.assertTrue(all(position.board.is_check() for position in by_prefix["checked"]))
        self.assertTrue(
            all(any(position.board.is_castling(move) for move in position.board.legal_moves)
                for position in by_prefix["castling"])
        )
        self.assertTrue(
            all(any(position.board.is_en_passant(move) for move in position.board.legal_moves)
                for position in by_prefix["enpassant"])
        )
        self.assertTrue(
            all(any(move.promotion for move in position.board.legal_moves)
                for position in by_prefix["promotion"])
        )
        self.assertTrue(
            all(len(position.board.piece_map()) <= 6 for position in by_prefix["lowmaterial"])
        )
        self.assertTrue(
            all(position.board.halfmove_clock >= 90 for position in by_prefix["rule50"])
        )


class ReportTests(unittest.TestCase):
    def test_generate_report_records_identity_settings_results_and_signature(self) -> None:
        with tempfile.TemporaryDirectory() as directory_name:
            directory = Path(directory_name)
            fake_engine = directory / "fake_engine.py"
            fake_engine.write_text(_FAKE_ENGINE, encoding="utf-8")
            command_log = directory / "commands.log"
            corpus = directory / "corpus.epd"
            corpus.write_text(
                'rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - '
                'id "start";\n',
                encoding="utf-8",
            )
            corpus_sha256 = hashlib.sha256(corpus.read_bytes()).hexdigest()
            command = [
                sys.executable,
                str(fake_engine),
                "normal",
                str(command_log),
            ]

            report = generate_report(
                command,
                corpus,
                nodes=100,
                threads=1,
                hash_mb=16,
                timeout=1.0,
            )

        self.assertEqual(
            report["engine_sha256"],
            hashlib.sha256(Path(sys.executable).read_bytes()).hexdigest(),
        )
        self.assertEqual(report["corpus_sha256"], corpus_sha256)
        self.assertEqual(report["uci"]["identity"]["name"], "Deterministic Fake")
        self.assertEqual(report["settings"], {"nodes": 100, "threads": 1, "hash_mb": 16})
        self.assertEqual(report["positions"][0]["id"], "start")
        self.assertEqual(report["positions"][0]["bestmove"], "e2e4")
        self.assertEqual(report["positions"][0]["nodes"], 100)
        self.assertEqual(report["aggregate"]["nodes"], 100)
        self.assertIn("time_ms", report["aggregate"])
        self.assertIn("nps", report["aggregate"])
        self.assertEqual(
            report["signature"], canonical_signature(signature_payload(report))
        )


if __name__ == "__main__":
    unittest.main()
