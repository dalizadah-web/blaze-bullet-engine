"""Regression tests proving incompatible experiments cannot be pooled.

These tests guard the integrity of the increment-cap qualification:
two runs with the same mutable ref text but different resolved commits,
mismatched binary hashes, mismatched opening hashes, or mismatched time
controls / SPRT parameters must never be aggregated together.
"""

from __future__ import annotations

import json
import tempfile
import unittest
from pathlib import Path

from tools.cloud_match.aggregate import aggregate_shards
from tools.cloud_match.spec import CloudMatchSpec


_COMMIT_C = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
_COMMIT_D = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"


def _base_payload() -> dict[str, object]:
    return {
        "schema_version": 2,
        "name": "id-test",
        "candidate_ref": "aa50f42331323ec06c05b4f5aa4d04437e3d57b9",
        "baseline_ref": "e5d7f7b",
        "candidate_commit": _COMMIT_C,
        "baseline_commit": _COMMIT_D,
        "candidate_sha256": "a" * 64,
        "baseline_sha256": "b" * 64,
        "games": 8,
        "shards": 2,
        "concurrency": 2,
        "time_control": "1+1",
        "threads": 1,
        "hash_mb": 16,
        "openings": "openings.epd",
        "opening_sha256": "c" * 64,
        "opening_start": 1,
        "opening_suite_positions": 4,
        "sprt": {"elo0": 0.0, "elo1": 5.0, "alpha": 0.05, "beta": 0.05},
    }


def _make_spec(payload: dict[str, object]) -> CloudMatchSpec:
    directory = tempfile.mkdtemp()
    path = Path(directory) / "spec.json"
    path.write_text(json.dumps(payload), encoding="utf-8")
    return CloudMatchSpec.from_json(path)


def _shard_manifest(spec: CloudMatchSpec, index: int, commit: str,
                    candidate_sha: str, opening_sha: str) -> Path:
    root = Path(tempfile.mkdtemp())
    shard = root / f"shard-{index}"
    shard.mkdir()
    (shard / "games.pgn").write_text("pgn", encoding="utf-8")
    pair_indexes = [index, index + 2]
    payload = {
        "schema_version": 3,
        "experiment_id": spec.experiment_id(),
        "shard_index": index,
        "shard_count": 2,
        "candidate_commit": commit,
        "baseline_commit": _COMMIT_D,
        "candidate_sha256": candidate_sha,
        "baseline_sha256": "b" * 64,
        "openings_sha256": opening_sha,
        "runner_sha256": "d" * 64,
        "expected_games": 4,
        "completed_games": 4,
        "clean_games": 4,
        "clean_pairs": 2,
        "quarantined_games": 0,
        "quarantined_pairs": 0,
        "raw_wdl": {"wins": 2, "draws": 2, "losses": 0},
        "clean_wdl": {"wins": 2, "draws": 2, "losses": 0},
        "pair_indexes": pair_indexes,
        "source_opening_indexes": [spec.opening_start + pair for pair in pair_indexes],
        "game_ids": [
            game_id
            for pair in pair_indexes
            for game_id in (
                f"{spec.experiment_id()}-p{pair:06d}-w",
                f"{spec.experiment_id()}-p{pair:06d}-b",
            )
        ],
        "counts": {
            "wins2": 1, "wins1_draw1": 0, "draws2": 1,
            "losses1_draw1": 0, "losses2": 0,
        },
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


class ExperimentIdentityPoolingTests(unittest.TestCase):
    def test_same_identity_pools(self) -> None:
        spec = _make_spec(_base_payload())
        first = _shard_manifest(spec, 0, spec.candidate_commit,
                                "a" * 64, spec.opening_sha256)
        second = _shard_manifest(spec, 1, spec.candidate_commit,
                                 "a" * 64, spec.opening_sha256)
        result = aggregate_shards([first, second], spec)
        self.assertEqual(result["clean_pairs"], 4)

    def test_different_resolved_commit_cannot_pool(self) -> None:
        spec = _make_spec(_base_payload())
        other = _make_spec(_base_payload())
        other = other.__class__(**{**other.__dict__,
                                   "candidate_commit": "f" * 40})
        first = _shard_manifest(spec, 0, spec.candidate_commit,
                                "a" * 64, spec.opening_sha256)
        second = _shard_manifest(other, 1, "f" * 40,
                                  "a" * 64, spec.opening_sha256)
        with self.assertRaisesRegex(ValueError, "experiment ID mismatch"):
            aggregate_shards([first, second], other)

    def test_mismatched_binary_hash_cannot_pool(self) -> None:
        spec = _make_spec(_base_payload())
        first = _shard_manifest(spec, 0, spec.candidate_commit,
                                "a" * 64, spec.opening_sha256)
        second = _shard_manifest(spec, 1, spec.candidate_commit,
                                  "e" * 64, spec.opening_sha256)
        with self.assertRaisesRegex(ValueError, "candidate_sha256 does not match frozen spec"):
            aggregate_shards([first, second], spec)

    def test_mismatched_opening_hash_cannot_pool(self) -> None:
        spec = _make_spec(_base_payload())
        first = _shard_manifest(spec, 0, spec.candidate_commit,
                                "a" * 64, spec.opening_sha256)
        second = _shard_manifest(spec, 1, spec.candidate_commit,
                                 "a" * 64, "f" * 64)
        with self.assertRaisesRegex(ValueError, "opening artifact mismatch"):
            aggregate_shards([first, second], spec)

    def test_mismatched_time_control_cannot_pool(self) -> None:
        spec = _make_spec(_base_payload())
        other_payload = _base_payload()
        other_payload["time_control"] = "2+1"
        other = _make_spec(other_payload)
        first = _shard_manifest(spec, 0, spec.candidate_commit,
                                "a" * 64, spec.opening_sha256)
        second = _shard_manifest(other, 1, other.candidate_commit,
                                  "a" * 64, other.opening_sha256)
        with self.assertRaisesRegex(ValueError, "experiment ID mismatch"):
            aggregate_shards([first, second], other)

    def test_mismatched_sprt_parameters_cannot_pool(self) -> None:
        spec = _make_spec(_base_payload())
        other_payload = _base_payload()
        other_payload["sprt"] = {"elo0": 0.0, "elo1": 10.0,
                                 "alpha": 0.05, "beta": 0.05}
        other = _make_spec(other_payload)
        first = _shard_manifest(spec, 0, spec.candidate_commit,
                                "a" * 64, spec.opening_sha256)
        second = _shard_manifest(other, 1, other.candidate_commit,
                                  "a" * 64, other.opening_sha256)
        with self.assertRaisesRegex(ValueError, "experiment ID mismatch"):
            aggregate_shards([first, second], other)

    def test_incomplete_shard_set_fails(self) -> None:
        spec = _make_spec(_base_payload())
        first = _shard_manifest(spec, 0, spec.candidate_commit,
                                "a" * 64, spec.opening_sha256)
        with self.assertRaisesRegex(ValueError, "expected 2 shard manifests"):
            aggregate_shards([first], spec)

    def test_duplicate_shard_set_fails(self) -> None:
        spec = _make_spec(_base_payload())
        first = _shard_manifest(spec, 0, spec.candidate_commit,
                                "a" * 64, spec.opening_sha256)
        second_dir = Path(tempfile.mkdtemp()) / "shard-dup"
        second_dir.mkdir()
        (second_dir / "games.pgn").write_text("pgn", encoding="utf-8")
        payload = json.loads(first.read_text(encoding="utf-8"))
        payload["shard_index"] = 0
        second = second_dir / "shard.json"
        second.write_text(json.dumps(payload), encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "duplicate shard index"):
            aggregate_shards([first, second], spec)


if __name__ == "__main__":
    unittest.main()
