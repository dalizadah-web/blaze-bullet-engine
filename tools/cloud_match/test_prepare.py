import json
from pathlib import Path
import tempfile
import unittest

from tools.cloud_match.prepare import prepare_spec


class PrepareSpecTests(unittest.TestCase):
    def test_overrides_dispatch_values_and_emits_matrix(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            base = root / "base.json"
            output = root / "prepared.json"
            base.write_text(
                json.dumps(
                    {
                    "schema_version": 2,
                    "name": "base",
                    "candidate_ref": "old",
                    "baseline_ref": "old-base",
                    "candidate_commit": "c" * 40,
                    "baseline_commit": "b" * 40,
                    "candidate_sha256": "",
                    "baseline_sha256": "",
                    "games": 12,
                    "shards": 2,
                    "concurrency": 2,
                    "time_control": "1+0",
                    "threads": 1,
                    "hash_mb": 16,
                    "openings": "book.epd",
                    "opening_sha256": "a" * 64,
                    "opening_start": 251,
                    "opening_suite_positions": 6,
                    "opening_repeats": 1,
                    "sprt": {"elo0": 0, "elo1": 5, "alpha": 0.05, "beta": 0.05},
                    }
                ),
                encoding="utf-8",
            )

            matrix = prepare_spec(
                base,
                output,
                candidate_ref="feature",
                baseline_ref="main",
                games=12,
                shards=3,
                time_control="0.5+0.01",
                threads=1,
                hash_mb=32,
                opening_start=101,
            )

            payload = json.loads(output.read_text(encoding="utf-8"))
            self.assertEqual(matrix, {"shard": [0, 1, 2]})
            self.assertEqual(payload["candidate_ref"], "feature")
            self.assertEqual(payload["games"], 12)
            self.assertEqual(payload["time_control"], "0.5+0.01")
            self.assertEqual(payload["opening_start"], 101)


if __name__ == "__main__":
    unittest.main()
