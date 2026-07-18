"""End-to-end integration test using the real tracked default config.

This exercises the full prepare -> resolve -> freeze -> aggregate path
against config/cloud/default-match.json so the production workflow cannot
regress silently behind synthetic fixtures.
"""

from __future__ import annotations

import json
import tempfile
import unittest
from pathlib import Path

from tools.cloud_match.aggregate import aggregate_shards
from tools.cloud_match.prepare import finalize_frozen_spec, prepare_spec
from tools.cloud_match.spec import CloudMatchSpec


REPO_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_CONFIG = REPO_ROOT / "config" / "cloud" / "default-match.json"


class DefaultConfigIntegrationTests(unittest.TestCase):
    def test_default_config_parses_and_resolves(self) -> None:
        # Stage 1: prepare resolves the mutable refs to Git SHA-1 commits.
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            prepared = root / "prepared.json"
            matrix = prepare_spec(
                DEFAULT_CONFIG,
                prepared,
                candidate_ref="aa50f42331323ec06c05b4f5aa4d04437e3d57b9",
                baseline_ref="e5d7f7b",
                games=10000,
                shards=20,
                opening_repeats=10,
                time_control="1+1",
                threads=1,
                hash_mb=16,
                repo_root=REPO_ROOT,
            )
            spec = CloudMatchSpec.from_json(prepared)
            self.assertEqual(matrix, {"shard": list(range(20))})
            # Resolved commits are real 40-char Git SHA-1 values.
            self.assertEqual(spec.candidate_commit, "aa50f42331323ec06c05b4f5aa4d04437e3d57b9")
            self.assertEqual(spec.baseline_commit, "e5d7f7b43b2629492e20439734f6bf446d19d807")
            self.assertEqual(len(spec.experiment_id()), 24)

            # Stage 2: freeze binary hashes after the build step.
            frozen = finalize_frozen_spec(
                prepared,
                candidate_sha256="c" * 64,
                baseline_sha256="d" * 64,
            )
            self.assertEqual(frozen.candidate_sha256, "c" * 64)
            self.assertEqual(frozen.baseline_sha256, "d" * 64)

            # Aggregation accepts a complete, consistent set of shards.
            shards = []
            for index in range(20):
                shard_dir = root / f"shard-{index}"
                shard_dir.mkdir()
                (shard_dir / "games.pgn").write_text("pgn", encoding="utf-8")
                pair_indexes = [p for p in range(5000) if p % 20 == index]
                pair_slots = [{"cycle": p // 500, "slot": p % 500} for p in pair_indexes]
                pair_count = len(pair_indexes)
                payload = {
                    "schema_version": 3,
                    "experiment_id": frozen.experiment_id(),
                    "shard_index": index,
                    "shard_count": 20,
                    "candidate_commit": frozen.candidate_commit,
                    "baseline_commit": frozen.baseline_commit,
                    "candidate_sha256": "c" * 64,
                    "baseline_sha256": "d" * 64,
                    "openings_sha256": frozen.opening_sha256,
                    "runner_sha256": "e" * 64,
                    "expected_games": len(pair_indexes) * 2,
                    "completed_games": len(pair_indexes) * 2,
                    "clean_games": len(pair_indexes) * 2,
                    "clean_pairs": pair_count,
                    "quarantined_games": 0,
                    "quarantined_pairs": 0,
                    "raw_wdl": {"wins": pair_count * 2, "draws": 0, "losses": 0},
                    "clean_wdl": {"wins": pair_count * 2, "draws": 0, "losses": 0},
                    "pair_indexes": pair_indexes,
                    "pair_slots": pair_slots,
                    "opening_repeats": frozen.opening_repeats,
                    "source_opening_indexes": [frozen.opening_start + item["slot"] for item in pair_slots],
                    "game_ids": [
                        game_id
                        for pair in pair_indexes
                        for game_id in (
                            f"{frozen.experiment_id()}-c{pair // 500:04d}-p{pair % 500:06d}-w",
                            f"{frozen.experiment_id()}-c{pair // 500:04d}-p{pair % 500:06d}-b",
                        )
                    ],
                    "counts": {
                        "wins2": pair_count, "wins1_draw1": 0, "draws2": 0,
                        "losses1_draw1": 0, "losses2": 0,
                    },
                    "termination_counts": {
                        "clean": {"ordinary": len(pair_indexes) * 2, "adjudication": 0},
                        "candidate": {"time_loss": 0, "illegal_move": 0, "disconnect": 0, "stall": 0},
                        "opponent": {"time_loss": 0, "illegal_move": 0, "disconnect": 0, "stall": 0},
                        "infrastructure_unknown": {"unterminated": 0, "malformed": 0, "unknown": 0, "contradictory": 0, "runner_failure": 0, "paired_quarantine": 0},
                    },
                    "abnormal_games": [],
                    "pgn": "games.pgn",
                }
                (shard_dir / "shard.json").write_text(
                    json.dumps(payload), encoding="utf-8"
                )
                shards.append(shard_dir / "shard.json")

            result = aggregate_shards(shards, frozen)
            self.assertEqual(result["expected_games"], 10000)
            self.assertEqual(result["clean_pairs"], 5000)


if __name__ == "__main__":
    unittest.main()
