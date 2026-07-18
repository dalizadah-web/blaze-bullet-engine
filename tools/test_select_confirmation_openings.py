from __future__ import annotations

import hashlib
import json
from pathlib import Path
import statistics
import tempfile
import unittest
from collections import Counter

import chess

from tools.select_confirmation_openings import _offer, select_openings


ROOT = Path(__file__).resolve().parents[1]
SUITE = ROOT / "testdata" / "openings" / "uho-lichess-500-v1.epd"
PROVENANCE = ROOT / "provenance" / "openings.json"


def _digest(tag: bytes, seed: int, line_number: int, line: bytes) -> bytes:
    return hashlib.sha256(
        tag
        + b"\0"
        + str(seed).encode("ascii")
        + b"\0"
        + str(line_number).encode("ascii")
        + b"\0"
        + line
    ).digest()


class ConfirmationSelectorTests(unittest.TestCase):
    def test_duplicate_updates_compact_the_lazy_heap_to_a_bounded_size(self) -> None:
        heap: list[tuple[int, int, bytes]] = []
        active: dict[bytes, tuple[bytes, int]] = {}
        line = b"8/8/8/8/8/8/4K3/7k w - - 0 1"
        for value in range(1000, 0, -1):
            _offer(
                heap,
                active,
                digest=value.to_bytes(32, "big"),
                line_number=value,
                line=line,
                quota=1,
            )
        self.assertLessEqual(len(heap), 4)
        self.assertEqual(len(active), 1)

    def test_verifies_the_source_archive_hash_before_selection(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            archive = root / "source.zip"
            archive.write_bytes(b"frozen archive bytes")
            source = root / "source.epd"
            source.write_text(
                "8/8/8/8/8/8/4K3/7k w - - 0 1\n"
                "8/8/8/8/8/8/4K3/7k b - - 0 1\n",
                encoding="ascii",
            )
            with self.assertRaisesRegex(ValueError, "source archive SHA-256 mismatch"):
                select_openings(
                    source,
                    root / "out.epd",
                    seed=1,
                    quota_per_side=1,
                    source_archive=archive,
                    expected_source_archive_sha256="0" * 64,
                )
            expected = hashlib.sha256(archive.read_bytes()).hexdigest()
            metadata = select_openings(
                source,
                root / "out.epd",
                seed=1,
                quota_per_side=1,
                source_archive=archive,
                expected_source_archive_sha256=expected,
            )
            self.assertEqual(metadata["source_archive_sha256"], expected)

    def test_streaming_selector_uses_frozen_priority_order_and_interleaves_sides(self) -> None:
        lines = [
            b"8/8/8/8/8/8/4K3/7k w - - 0 1",
            b"8/8/8/8/8/8/4K3/7k b - - 0 1",
            b"8/8/8/8/8/8/3K4/7k w - - 0 1",
            b"8/8/8/8/8/8/3K4/7k b - - 0 1",
            b"8/8/8/8/8/8/2K5/7k w - - 0 1",
            b"8/8/8/8/8/8/2K5/7k b - - 0 1",
        ]
        source_bytes = b"\n".join(lines) + b"\n"
        expected_by_side: dict[bytes, list[tuple[bytes, int, bytes]]] = {b"w": [], b"b": []}
        for number, line in enumerate(lines, 1):
            side = line.split()[1]
            expected_by_side[side].append((_digest(b"select", 7, number, line), number, line))
        chosen: dict[bytes, list[tuple[bytes, int, bytes]]] = {}
        for side in (b"w", b"b"):
            lowest = sorted(expected_by_side[side])[:2]
            chosen[side] = sorted(
                lowest,
                key=lambda item: (_digest(b"order", 7, item[1], item[2]), item[1], item[2]),
            )
        expected = b"".join(
            chosen[side][index][2] + b"\n"
            for index in range(2)
            for side in (b"w", b"b")
        )

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "source.epd"
            output = root / "output.epd"
            source.write_bytes(source_bytes)
            metadata = select_openings(source, output, seed=7, quota_per_side=2)

            self.assertEqual(output.read_bytes(), expected)
            self.assertEqual(metadata["source_nonempty_lines"], 6)
            self.assertEqual(metadata["selected_source_lines"], [
                item[1] for index in range(2) for item in (chosen[b"w"][index], chosen[b"b"][index])
            ])

    def test_rejects_frozen_source_hash_and_count_mismatches(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "source.epd"
            source.write_text(
                "8/8/8/8/8/8/4K3/7k w - - 0 1\n"
                "8/8/8/8/8/8/4K3/7k b - - 0 1\n",
                encoding="ascii",
            )
            with self.assertRaisesRegex(ValueError, "source SHA-256 mismatch"):
                select_openings(
                    source,
                    root / "out.epd",
                    seed=1,
                    quota_per_side=1,
                    expected_source_sha256="0" * 64,
                )

    def test_duplicate_source_lines_use_best_occurrence_and_do_not_consume_quota(self) -> None:
        white_a = b"8/8/8/8/8/8/4K3/7k w - - 0 1"
        white_b = b"8/8/8/8/8/8/3K4/7k w - - 0 1"
        black_a = b"8/8/8/8/8/8/4K3/7k b - - 0 1"
        black_b = b"8/8/8/8/8/8/3K4/7k b - - 0 1"
        source_lines = [white_a, black_a, white_a, black_a, white_b, black_b]
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "source.epd"
            output = root / "out.epd"
            source.write_bytes(b"\n".join(source_lines) + b"\n")

            metadata = select_openings(source, output, seed=1, quota_per_side=2)

            self.assertEqual(set(output.read_bytes().splitlines()), {white_a, white_b, black_a, black_b})
            duplicate_best = {
                min(
                    (number for number in (1, 3)),
                    key=lambda number: (_digest(b"select", 1, number, white_a), number, white_a),
                ),
                min(
                    (number for number in (2, 4)),
                    key=lambda number: (_digest(b"select", 1, number, black_a), number, black_a),
                ),
            }
            self.assertTrue(duplicate_best.issubset(set(metadata["selected_source_lines"])))
            with self.assertRaisesRegex(ValueError, "source nonempty line count mismatch"):
                select_openings(
                    source,
                    root / "out.epd",
                    seed=1,
                    quota_per_side=1,
                    expected_source_nonempty_lines=3,
                )

    def test_committed_suite_matches_frozen_hash_provenance_and_chess_invariants(self) -> None:
        raw = SUITE.read_bytes()
        lines = raw.splitlines()
        provenance = json.loads(PROVENANCE.read_text(encoding="utf-8"))
        entry = next(item for item in provenance["opening_suites"] if item["path"].endswith(SUITE.name))

        self.assertEqual(hashlib.sha256(raw).hexdigest(), "5a53816436fe460d788fe1334fc9be27c89ee9bc1d0bdb1ab9745e3081d404bc")
        self.assertEqual(entry["sha256"], hashlib.sha256(raw).hexdigest())
        self.assertEqual(entry["positions"], 500)
        self.assertEqual(entry["selection"]["ordered_source_line_sha256"], "b72aab2191b51dd823792d16febce220d1c447646153c5ede959a426387f8eb6")
        self.assertEqual(len(entry["selection"]["source_line_numbers"]), 500)
        self.assertEqual(len(set(entry["selection"]["source_line_numbers"])), 500)
        source_number_bytes = b"".join(
            str(number).encode("ascii") + b"\n"
            for number in entry["selection"]["source_line_numbers"]
        )
        self.assertEqual(
            hashlib.sha256(source_number_bytes).hexdigest(),
            entry["selection"]["ordered_source_line_sha256"],
        )
        self.assertEqual(entry["source"]["commit"], "65815ccdbc7727cd4f6aee252ba8f67fb740e92f")
        self.assertEqual(entry["source"]["archive_sha256"], "4e298f11e8acfa106babe02968f2e61582145e7874c59284690b20b9650e0e07")
        self.assertEqual(entry["source"]["uncompressed_sha256"], "7a7f6470615a69c6cf23d565417701d38732876f480af90d67b42abade35644a")
        self.assertEqual(entry["source"]["nonempty_lines"], 2_632_036)
        self.assertEqual(entry["source"]["side_to_move"], {"white": 1_457_270, "black": 1_174_766})
        self.assertEqual(len(lines), 500)
        self.assertEqual(len(set(lines)), 500)
        boards = [chess.Board(line.decode("ascii")) for line in lines]
        self.assertTrue(all(board.is_valid() for board in boards))
        self.assertTrue(all(not board.is_game_over(claim_draw=False) for board in boards))
        plies = [
            (board.fullmove_number - 1) * 2 + (0 if board.turn == chess.WHITE else 1)
            for board in boards
        ]
        self.assertEqual((min(plies), max(plies), statistics.median(plies)), (4, 16, 14))
        self.assertEqual(
            {str(key): value for key, value in sorted(Counter(plies).items())},
            entry["validation"]["ply"]["distribution"],
        )
        for lane in (boards[:250], boards[250:]):
            self.assertEqual(sum(board.turn == chess.WHITE for board in lane), 125)
            self.assertEqual(sum(board.turn == chess.BLACK for board in lane), 125)

    def test_no_production_strength_config_uses_the_smoke_suite(self) -> None:
        config_root = ROOT / "config"
        for path in sorted(config_root.rglob("*.json")):
            with self.subTest(path=path.relative_to(ROOT)):
                payload = json.loads(path.read_text(encoding="utf-8"))
                self.assertNotIn("smoke-v1", str(payload.get("openings", "")))
                self.assertEqual(payload.get("opening_sha256"), "5a53816436fe460d788fe1334fc9be27c89ee9bc1d0bdb1ab9745e3081d404bc")
                self.assertIsInstance(payload.get("opening_start"), int)
                self.assertEqual(payload.get("opening_suite_positions"), 500)
                self.assertLessEqual(
                    payload["opening_start"] + payload["games"] // 2 - 1,
                    500,
                )

    def test_opening_artifacts_are_checked_out_with_hash_stable_lf_bytes(self) -> None:
        attributes = (ROOT / ".gitattributes").read_text(encoding="utf-8")
        self.assertIn("testdata/openings/*.epd text eol=lf", attributes.splitlines())


if __name__ == "__main__":
    unittest.main()
