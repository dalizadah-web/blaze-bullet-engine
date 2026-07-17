import json
from pathlib import Path
import tempfile
import unittest

from tools.cloud_match.aggregate import aggregate_shards
from tools.cloud_match.spec import CloudMatchSpec


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
        payload = {
            "schema_version": 1,
            "experiment_id": self.spec.experiment_id(),
            "shard_index": index,
            "shard_count": 2,
            "candidate_sha256": "a" * 64,
            "baseline_sha256": "b" * 64,
            "openings_sha256": "c" * 64,
            "runner_sha256": "d" * 64,
            "expected_games": 4,
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

        self.assertEqual(result["games"], 8)
        self.assertEqual(result["pairs"], 4)
        self.assertEqual(result["counts"], {
            "wins2": 1,
            "wins1_draw1": 1,
            "draws2": 1,
            "losses1_draw1": 0,
            "losses2": 1,
        })
        self.assertIn(result["decision"], ("accept", "reject", "continue"))

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


if __name__ == "__main__":
    unittest.main()
