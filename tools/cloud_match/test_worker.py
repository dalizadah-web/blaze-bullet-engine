from pathlib import Path
import tempfile
import unittest

from tools.cloud_match.shards import game_ids_for_slots
from tools.cloud_match.worker import (
    _globalize_evidence,
    run_worker,
    write_shard_openings,
)
from tools.experiment.match import MatchEvidence
from tools.experiment.pentanomial import Pentanomial


class WorkerOpeningTests(unittest.TestCase):
    def test_globalizes_abnormal_records_without_changing_evidence_arithmetic(self) -> None:
        evidence = MatchEvidence(
            expected_games=2,
            completed_games=2,
            clean_games=0,
            clean_pairs=0,
            quarantined_games=2,
            quarantined_pairs=1,
            raw_wdl={"wins": 0, "draws": 1, "losses": 1},
            clean_wdl={"wins": 0, "draws": 0, "losses": 0},
            counts=Pentanomial(0, 0, 0, 0, 0),
            termination_counts={
                "clean": {"ordinary": 1, "adjudication": 0},
                "candidate": {"time_loss": 1, "illegal_move": 0, "disconnect": 0, "stall": 0},
                "opponent": {"time_loss": 0, "illegal_move": 0, "disconnect": 0, "stall": 0},
                "infrastructure_unknown": {"unterminated": 0, "malformed": 0, "unknown": 0, "contradictory": 0, "runner_failure": 0, "paired_quarantine": 1},
            },
            abnormal_games=({
                "game_index": 0,
                "round": "1",
                "result": "0-1",
                "candidate_color": "white",
                "termination": "time forfeit",
                "category": "time_loss",
                "offender": "candidate",
                "reason": "engine failure",
            }, {
                "game_index": 1,
                "round": "1",
                "result": "1/2-1/2",
                "candidate_color": "black",
                "termination": "",
                "category": "paired_quarantine",
                "offender": "unknown",
                "reason": "color-paired game was quarantined",
            }),
        )

        payload = _globalize_evidence(evidence, ["experiment-p000000-w", "experiment-p000000-b"])

        self.assertEqual(payload["schema_version"], 3)
        self.assertEqual(payload["quarantined_pairs"], 1)
        self.assertEqual(payload["abnormal_games"][0]["game_id"], "experiment-p000000-w")
        self.assertNotIn("game_index", payload["abnormal_games"][0])

    def test_globalizes_by_round_and_candidate_color_not_record_order(self) -> None:
        evidence = MatchEvidence(
            expected_games=2,
            completed_games=0,
            clean_games=0,
            clean_pairs=0,
            quarantined_games=2,
            quarantined_pairs=1,
            raw_wdl={"wins": 0, "draws": 0, "losses": 0},
            clean_wdl={"wins": 0, "draws": 0, "losses": 0},
            counts=Pentanomial(0, 0, 0, 0, 0),
            termination_counts={
                "clean": {"ordinary": 0, "adjudication": 0},
                "candidate": {"time_loss": 0, "illegal_move": 0, "disconnect": 0, "stall": 0},
                "opponent": {"time_loss": 0, "illegal_move": 0, "disconnect": 0, "stall": 0},
                "infrastructure_unknown": {"unterminated": 2, "malformed": 0, "unknown": 0, "contradictory": 0, "runner_failure": 0, "paired_quarantine": 0},
            },
            abnormal_games=(
                {"game_index": 0, "round": "1", "candidate_color": "black"},
                {"game_index": 1, "round": "1", "candidate_color": "white"},
            ),
        )

        payload = _globalize_evidence(evidence, ["round1-white", "round1-black"])

        self.assertEqual(
            [record["game_id"] for record in payload["abnormal_games"]],
            ["round1-black", "round1-white"],
        )

    def test_selects_deterministic_cyclic_openings_for_shard(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "openings.epd"
            source.write_text("fen-0\n\nfen-1\nfen-2\n", encoding="utf-8")
            destination = root / "shard.epd"

            selected = write_shard_openings(source, [0, 1, 2], destination)

            self.assertEqual(selected, ["fen-0", "fen-1", "fen-2"])
            self.assertEqual(destination.read_text(encoding="utf-8"), "fen-0\nfen-1\nfen-2\n")

    def test_rejects_empty_opening_source(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "openings.epd"
            source.write_text("\n", encoding="utf-8")

            with self.assertRaisesRegex(ValueError, "no opening positions"):
                write_shard_openings(source, [0], root / "shard.epd")

    def test_applies_one_based_lane_offset_without_wraparound(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "openings.epd"
            source.write_text("a\nb\nc\nd\ne\n", encoding="utf-8")

            selected = write_shard_openings(
                source, [0, 2], root / "shard.epd", opening_start=2
            )
            self.assertEqual(selected, ["b", "d"])

            with self.assertRaisesRegex(ValueError, "outside opening source"):
                write_shard_openings(
                    source, [0, 4], root / "bad.epd", opening_start=2
                )
            with self.assertRaisesRegex(ValueError, "duplicate opening source index"):
                write_shard_openings(
                    source, [1, 1], root / "duplicate.epd", opening_start=2
                )

    def test_repeats_source_openings_only_when_explicitly_enabled(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "openings.epd"
            source.write_text("a\nb\n", encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "duplicate opening source index"):
                write_shard_openings(source, [0, 1, 0], root / "no-repeat.epd")
            selected = write_shard_openings(
                source, [0, 1, 0], root / "repeat.epd", allow_duplicates=True
            )
            self.assertEqual(selected, ["a", "b", "a"])

    def test_cycle_qualified_ids_are_unique_and_one_cycle_ids_are_legacy_stable(self) -> None:
        repeated = game_ids_for_slots(
            "experiment",
            [(0, 0), (1, 0)],
            include_cycle=True,
        )
        self.assertEqual(len(repeated), len(set(repeated)))
        self.assertIn("experiment-c0000-p000000-w", repeated)
        self.assertIn("experiment-c0001-p000000-w", repeated)
        self.assertEqual(
            game_ids_for_slots("experiment", [(0, 0)], include_cycle=False),
            ["experiment-p000000-w", "experiment-p000000-b"],
        )

    def test_rejects_a_spec_without_frozen_binary_hashes(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            spec = root / "spec.json"
            spec.write_text(
                """{
  \"schema_version\": 2,
  \"name\": \"test\",
  \"candidate_ref\": \"candidate\",
  \"baseline_ref\": \"baseline\",
  \"candidate_commit\": \"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",
  \"baseline_commit\": \"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\",
  \"candidate_sha256\": \"\",
  \"baseline_sha256\": \"\",
  \"games\": 2,
  \"shards\": 1,
  \"concurrency\": 1,
  \"time_control\": \"1+0\",
  \"threads\": 1,
  \"hash_mb\": 16,
  \"openings\": \"openings.epd\",
  \"opening_sha256\": \"cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc\",
  \"opening_start\": 1,
  \"opening_suite_positions\": 1,
  \"opening_repeats\": 1,
  \"sprt\": {\"elo0\": 0, \"elo1\": 5, \"alpha\": 0.05, \"beta\": 0.05}
}""",
                encoding="utf-8",
            )

            with self.assertRaisesRegex(ValueError, "candidate_sha256 must be a 64-char SHA-256 digest"):
                run_worker(
                    spec_path=spec,
                    shard_index=0,
                    candidate=root / "candidate",
                    baseline=root / "baseline",
                    runner=root / "runner",
                    output=root / "output",
                    source_commit="a" * 40,
                )


if __name__ == "__main__":
    unittest.main()
