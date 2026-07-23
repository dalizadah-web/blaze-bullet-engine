import json
from pathlib import Path
import tempfile
import unittest

from tools.cloud_match.spec import CloudMatchSpec


def valid_payload() -> dict[str, object]:
    return {
        "schema_version": 2,
        "name": "bullet-regression",
        "candidate_ref": "codex/bullet-beast",
        "baseline_ref": "4d25363fef79ff2025670e248ed07b3d81747d3a",
        "candidate_commit": "aa50f42331323ec06c05b4f5aa4d04437e3d57b9",
        "baseline_commit": "4d25363fef79ff2025670e248ed07b3d81747d3a",
        "candidate_sha256": "",
        "baseline_sha256": "",
        "games": 400,
        "shards": 40,
        "concurrency": 2,
        "time_control": "0.5+0",
        "threads": 1,
        "hash_mb": 16,
        "openings": "testdata/openings/smoke-v1.epd",
        "opening_sha256": "a" * 64,
        "opening_start": 1,
        "opening_suite_positions": 200,
        "opening_repeats": 1,
        "sprt": {"elo0": 0.0, "elo1": 5.0, "alpha": 0.05, "beta": 0.05},
    }


class CloudMatchSpecTests(unittest.TestCase):
    def test_requires_frozen_full_suite_size(self) -> None:
        payload = valid_payload()
        payload.pop("opening_suite_positions", None)
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "spec.json"
            path.write_text(json.dumps(payload), encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "opening_suite_positions"):
                CloudMatchSpec.from_json(path)

    def test_requires_explicit_positive_opening_repeats(self) -> None:
        for value in (None, 0, -1, True):
            payload = valid_payload()
            if value is None:
                payload.pop("opening_repeats")
            else:
                payload["opening_repeats"] = value
            with self.subTest(value=value), tempfile.TemporaryDirectory() as directory:
                path = Path(directory) / "spec.json"
                path.write_text(json.dumps(payload), encoding="utf-8")
                with self.assertRaisesRegex(ValueError, "opening_repeats"):
                    CloudMatchSpec.from_json(path)

    def test_rejects_non_exact_repeat_geometry(self) -> None:
        payload = valid_payload()
        payload["opening_repeats"] = 2
        with self.assertRaisesRegex(ValueError, "games/2"):
            self.parse(payload)

    def parse(self, payload: dict[str, object]) -> CloudMatchSpec:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "match.json"
            path.write_text(json.dumps(payload), encoding="utf-8")
            return CloudMatchSpec.from_json(path)

    def test_accepts_a_forty_shard_public_match(self) -> None:
        spec = self.parse(valid_payload())
        self.assertEqual(spec.games, 400)
        self.assertEqual(spec.shards, 40)
        self.assertEqual(spec.opening_start, 1)
        self.assertEqual(len(spec.experiment_id()), 24)

    def test_rejects_legacy_spec_without_range_identity(self) -> None:
        payload = valid_payload()
        payload["schema_version"] = 1
        with self.assertRaisesRegex(ValueError, "schema_version must be 2"):
            self.parse(payload)

    def test_rejects_unsafe_or_unpairable_inputs(self) -> None:
        cases = (
            ("games", 399),
            ("shards", 0),
            ("shards", 41),
            ("concurrency", 3),
            ("threads", 3),
            ("opening_sha256", "bad"),
            ("opening_start", 0),
            ("opening_repeats", 0),
        )
        for field, value in cases:
            with self.subTest(field=field, value=value):
                payload = valid_payload()
                payload[field] = value
                with self.assertRaises(ValueError):
                    self.parse(payload)


if __name__ == "__main__":
    unittest.main()
