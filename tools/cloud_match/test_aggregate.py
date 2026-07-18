import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from tools.cloud_match.aggregate import aggregate_shards, summary_markdown
from tools.cloud_match.spec import CloudMatchSpec


_COMMIT_C = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
_COMMIT_D = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"


class AggregateShardsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.spec_path = self.root / "spec.json"
        self.spec_path.write_text(
            json.dumps(
                {
                    "schema_version": 1,
                    "name": "test",
                    "candidate_ref": "candidate",
                    "baseline_ref": "baseline",
                    "candidate_commit": _COMMIT_C,
                    "baseline_commit": _COMMIT_D,
                    "candidate_sha256": "a" * 64,
                    "baseline_sha256": "b" * 64,
                    "games": 8,
                    "shards": 2,
                    "concurrency": 2,
                    "time_control": "1+0",
                    "threads": 1,
                    "hash_mb": 16,
                    "openings": "openings.epd",
                    "opening_sha256": "c" * 64,
                    "sprt": {"elo0": 0, "elo1": 5, "alpha": 0.05, "beta": 0.05},
                }
            ),
            encoding="utf-8",
        )
        self.spec = CloudMatchSpec.from_json(self.spec_path)

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def _write_shard(self, index: int, counts: dict[str, int]) -> Path:
        shard = self.root / f"shard-{index}"
        shard.mkdir()
        (shard / "games.pgn").write_text("pgn", encoding="utf-8")
        pair_indexes = [index, index + 2]
        wins = counts["wins2"] * 2 + counts["wins1_draw1"]
        draws = counts["wins1_draw1"] + counts["draws2"] * 2 + counts["losses1_draw1"]
        losses = counts["losses1_draw1"] + counts["losses2"] * 2
        payload = {
            "schema_version": 3,
            "experiment_id": self.spec.experiment_id(),
            "shard_index": index,
            "shard_count": 2,
            "candidate_commit": _COMMIT_C,
            "baseline_commit": _COMMIT_D,
            "candidate_sha256": "a" * 64,
            "baseline_sha256": "b" * 64,
            "openings_sha256": "c" * 64,
            "runner_sha256": "d" * 64,
            "expected_games": 4,
            "completed_games": 4,
            "clean_games": 4,
            "clean_pairs": 2,
            "quarantined_games": 0,
            "quarantined_pairs": 0,
            "raw_wdl": {"wins": wins, "draws": draws, "losses": losses},
            "clean_wdl": {"wins": wins, "draws": draws, "losses": losses},
            "pair_indexes": pair_indexes,
            "game_ids": [
                game_id
                for pair in pair_indexes
                for game_id in (
                    f"{self.spec.experiment_id()}-p{pair:06d}-w",
                    f"{self.spec.experiment_id()}-p{pair:06d}-b",
                )
            ],
            "counts": counts,
            "termination_counts": {
                "clean": {"ordinary": 4, "adjudication": 0},
                "candidate": {"time_loss": 0, "illegal_move": 0, "disconnect": 0, "stall": 0},
                "opponent": {"time_loss": 0, "illegal_move": 0, "disconnect": 0, "stall": 0},
                "infrastructure_unknown": {"unterminated": 0, "malformed": 0, "unknown": 0, "contradictory": 0, "runner_failure": 0, "paired_quarantine": 0},
            },
            "abnormal_games": [],
            "pgn": "games.pgn",
        }
        manifest = shard / "shard.json"
        manifest.write_text(json.dumps(payload), encoding="utf-8")
        return manifest

    def test_aggregates_complete_consistent_shards(self) -> None:
        first = self._write_shard(
            0,
            {"wins2": 1, "wins1_draw1": 0, "draws2": 1, "losses1_draw1": 0, "losses2": 0},
        )
        second = self._write_shard(
            1,
            {"wins2": 0, "wins1_draw1": 1, "draws2": 0, "losses1_draw1": 0, "losses2": 1},
        )

        result = aggregate_shards([first, second], self.spec)

        self.assertEqual(result["expected_games"], 8)
        self.assertEqual(result["clean_pairs"], 4)
        self.assertEqual(result["counts"], {
            "wins2": 1,
            "wins1_draw1": 1,
            "draws2": 1,
            "losses1_draw1": 0,
            "losses2": 1,
        })
        self.assertIn(result["decision"], ("accept", "reject", "continue"))

    def test_quarantines_abnormal_pair_and_rejects_forged_termination_counts(self) -> None:
        first = self._write_shard(
            0,
            {"wins2": 1, "wins1_draw1": 0, "draws2": 0, "losses1_draw1": 0, "losses2": 0},
        )
        payload = json.loads(first.read_text(encoding="utf-8"))
        payload.update({
            "clean_games": 2,
            "clean_pairs": 1,
            "quarantined_games": 2,
            "quarantined_pairs": 1,
            "raw_wdl": {"wins": 2, "draws": 1, "losses": 1},
            "clean_wdl": {"wins": 2, "draws": 0, "losses": 0},
        })
        payload["termination_counts"]["clean"]["ordinary"] = 2
        payload["termination_counts"]["candidate"]["time_loss"] = 1
        payload["termination_counts"]["infrastructure_unknown"]["paired_quarantine"] = 1
        payload["abnormal_games"] = [{
            "game_id": payload["game_ids"][2],
            "round": "3",
            "result": "0-1",
            "candidate_color": "white",
            "termination": "time forfeit",
            "category": "time_loss",
            "offender": "candidate",
            "reason": "engine failure",
        }, {
            "game_id": payload["game_ids"][3],
            "round": "4",
            "result": "1/2-1/2",
            "candidate_color": "black",
            "termination": "",
            "category": "paired_quarantine",
            "offender": "unknown",
            "reason": "color-paired game was quarantined",
        }]
        first.write_text(json.dumps(payload), encoding="utf-8")
        second = self._write_shard(
            1,
            {"wins2": 0, "wins1_draw1": 0, "draws2": 2, "losses1_draw1": 0, "losses2": 0},
        )

        result = aggregate_shards([first, second], self.spec)

        self.assertEqual(result["expected_games"], 8)
        self.assertEqual(result["clean_games"], 6)
        self.assertEqual(result["quarantined_pairs"], 1)
        self.assertEqual(result["termination_counts"]["candidate"]["time_loss"], 1)
        self.assertEqual(result["abnormal_games"][0]["game_id"], payload["game_ids"][2])
        markdown = summary_markdown(result)
        self.assertIn("Candidate time_loss: 1", markdown)
        self.assertIn(payload["game_ids"][2], markdown)

        forged = json.loads(first.read_text(encoding="utf-8"))
        forged["termination_counts"]["candidate"]["time_loss"] = 2
        first.write_text(json.dumps(forged), encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "termination count"):
            aggregate_shards([first, second], self.spec)

        contradictory = json.loads(json.dumps(payload))
        contradictory["abnormal_games"][0]["result"] = "1-0"
        first.write_text(json.dumps(contradictory), encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "contradictory offender/result"):
            aggregate_shards([first, second], self.spec)

        forged_clean = json.loads(json.dumps(payload))
        forged_clean["clean_wdl"] = {"wins": 1, "draws": 1, "losses": 0}
        first.write_text(json.dumps(forged_clean), encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "clean W/D/L contradicts"):
            aggregate_shards([first, second], self.spec)

        forged_raw = json.loads(json.dumps(payload))
        forged_raw["raw_wdl"] = {"wins": 1, "draws": 2, "losses": 1}
        first.write_text(json.dumps(forged_raw), encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "raw W/D/L does not reconcile"):
            aggregate_shards([first, second], self.spec)

    def test_zero_clean_shards_never_call_sprt(self) -> None:
        manifests = [
            self._write_shard(index, {"wins2": 0, "wins1_draw1": 0, "draws2": 2, "losses1_draw1": 0, "losses2": 0})
            for index in (0, 1)
        ]
        for manifest in manifests:
            payload = json.loads(manifest.read_text(encoding="utf-8"))
            payload.update({
                "clean_games": 0,
                "clean_pairs": 0,
                "quarantined_games": 4,
                "quarantined_pairs": 2,
                "raw_wdl": {"wins": 0, "draws": 4, "losses": 0},
                "clean_wdl": {"wins": 0, "draws": 0, "losses": 0},
                "counts": {"wins2": 0, "wins1_draw1": 0, "draws2": 0, "losses1_draw1": 0, "losses2": 0},
            })
            payload["termination_counts"]["clean"]["ordinary"] = 0
            payload["termination_counts"]["infrastructure_unknown"]["paired_quarantine"] = 4
            payload["abnormal_games"] = [
                {
                    "game_id": game_id,
                    "round": str(i + 1),
                    "result": "1/2-1/2",
                    "candidate_color": "white" if i % 2 == 0 else "black",
                    "termination": "",
                    "category": "paired_quarantine",
                    "offender": "unknown",
                    "reason": "quarantined",
                }
                for i, game_id in enumerate(payload["game_ids"])
            ]
            manifest.write_text(json.dumps(payload), encoding="utf-8")

        with patch("tools.cloud_match.aggregate.sprt_llr", side_effect=AssertionError("SPRT called")), patch(
            "tools.cloud_match.aggregate.sprt_decision", side_effect=AssertionError("SPRT called")
        ):
            result = aggregate_shards(manifests, self.spec)

        self.assertEqual(result["decision"], "no_clean_pairs")
        self.assertEqual(result["llr"], 0.0)

    def test_rejects_backward_incompatible_shard_schema(self) -> None:
        first = self._write_shard(0, {"wins2": 2, "wins1_draw1": 0, "draws2": 0, "losses1_draw1": 0, "losses2": 0})
        second = self._write_shard(1, {"wins2": 2, "wins1_draw1": 0, "draws2": 0, "losses1_draw1": 0, "losses2": 0})
        payload = json.loads(first.read_text(encoding="utf-8"))
        payload["schema_version"] = 2
        first.write_text(json.dumps(payload), encoding="utf-8")

        with self.assertRaisesRegex(ValueError, "unsupported shard schema"):
            aggregate_shards([first, second], self.spec)

    def test_rejects_duplicate_game_ids(self) -> None:
        first = self._write_shard(0, {"wins2": 2, "wins1_draw1": 0, "draws2": 0, "losses1_draw1": 0, "losses2": 0})
        second = self._write_shard(1, {"wins2": 2, "wins1_draw1": 0, "draws2": 0, "losses1_draw1": 0, "losses2": 0})
        payload = json.loads(second.read_text(encoding="utf-8"))
        payload["game_ids"][0] = json.loads(first.read_text(encoding="utf-8"))["game_ids"][0]
        second.write_text(json.dumps(payload), encoding="utf-8")

        with self.assertRaisesRegex(ValueError, "duplicate game ID"):
            aggregate_shards([first, second], self.spec)

    def test_rejects_missing_shard(self) -> None:
        first = self._write_shard(0, {"wins2": 2, "wins1_draw1": 0, "draws2": 0, "losses1_draw1": 0, "losses2": 0})

        with self.assertRaisesRegex(ValueError, "expected 2 shard manifests"):
            aggregate_shards([first], self.spec)

    def test_rejects_mismatched_candidate_commit(self) -> None:
        first = self._write_shard(0, {"wins2": 2, "wins1_draw1": 0, "draws2": 0, "losses1_draw1": 0, "losses2": 0})
        second = self._write_shard(1, {"wins2": 0, "wins1_draw1": 0, "draws2": 0, "losses1_draw1": 0, "losses2": 2})
        payload = json.loads(second.read_text(encoding="utf-8"))
        payload["candidate_commit"] = "f" * 40
        second.write_text(json.dumps(payload), encoding="utf-8")

        with self.assertRaisesRegex(ValueError, "candidate_commit does not match frozen spec"):
            aggregate_shards([first, second], self.spec)

    def test_rejects_mismatched_baseline_sha256(self) -> None:
        first = self._write_shard(0, {"wins2": 2, "wins1_draw1": 0, "draws2": 0, "losses1_draw1": 0, "losses2": 0})
        second = self._write_shard(1, {"wins2": 0, "wins1_draw1": 0, "draws2": 0, "losses1_draw1": 0, "losses2": 2})
        payload = json.loads(second.read_text(encoding="utf-8"))
        payload["baseline_sha256"] = "f" * 64
        second.write_text(json.dumps(payload), encoding="utf-8")

        with self.assertRaisesRegex(ValueError, "baseline_sha256 does not match frozen spec"):
            aggregate_shards([first, second], self.spec)

    def test_rejects_consistent_candidate_hash_that_differs_from_frozen_spec(self) -> None:
        first = self._write_shard(0, {"wins2": 2, "wins1_draw1": 0, "draws2": 0, "losses1_draw1": 0, "losses2": 0})
        second = self._write_shard(1, {"wins2": 0, "wins1_draw1": 0, "draws2": 0, "losses1_draw1": 0, "losses2": 2})
        for manifest in (first, second):
            payload = json.loads(manifest.read_text(encoding="utf-8"))
            payload["candidate_sha256"] = "f" * 64
            manifest.write_text(json.dumps(payload), encoding="utf-8")

        with self.assertRaisesRegex(ValueError, "candidate_sha256 does not match frozen spec"):
            aggregate_shards([first, second], self.spec)

    def test_rejects_consistent_candidate_commit_that_differs_from_frozen_spec(self) -> None:
        first = self._write_shard(0, {"wins2": 2, "wins1_draw1": 0, "draws2": 0, "losses1_draw1": 0, "losses2": 0})
        second = self._write_shard(1, {"wins2": 0, "wins1_draw1": 0, "draws2": 0, "losses1_draw1": 0, "losses2": 2})
        for manifest in (first, second):
            payload = json.loads(manifest.read_text(encoding="utf-8"))
            payload["candidate_commit"] = "f" * 40
            manifest.write_text(json.dumps(payload), encoding="utf-8")

        with self.assertRaisesRegex(ValueError, "candidate_commit does not match frozen spec"):
            aggregate_shards([first, second], self.spec)

    def test_rejects_duplicate_shard_index(self) -> None:
        first = self._write_shard(0, {"wins2": 2, "wins1_draw1": 0, "draws2": 0, "losses1_draw1": 0, "losses2": 0})
        second_dir = self.root / "shard-dup0"
        second_dir.mkdir()
        (second_dir / "games.pgn").write_text("pgn", encoding="utf-8")
        pair_indexes = [0, 2]
        payload = {
            "schema_version": 3,
            "experiment_id": self.spec.experiment_id(),
            "shard_index": 0,
            "shard_count": 2,
            "candidate_commit": _COMMIT_C,
            "baseline_commit": _COMMIT_D,
            "candidate_sha256": "a" * 64,
            "baseline_sha256": "b" * 64,
            "openings_sha256": "c" * 64,
            "runner_sha256": "d" * 64,
            "expected_games": 4,
            "pair_indexes": pair_indexes,
            "game_ids": [
                f"{self.spec.experiment_id()}-p{pair:06d}-w" for pair in pair_indexes
            ] + [
                f"{self.spec.experiment_id()}-p{pair:06d}-b" for pair in pair_indexes
            ],
            "counts": {"wins2": 0, "wins1_draw1": 0, "draws2": 0, "losses1_draw1": 0, "losses2": 2},
            "pgn": "games.pgn",
        }
        second = second_dir / "shard.json"
        second.write_text(json.dumps(payload), encoding="utf-8")

        with self.assertRaisesRegex(ValueError, "duplicate shard index"):
            aggregate_shards([first, second], self.spec)


if __name__ == "__main__":
    unittest.main()
