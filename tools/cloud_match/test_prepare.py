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
                        "schema_version": 1,
                        "name": "base",
                        "candidate_ref": "old",
                        "baseline_ref": "old-base",
                        "games": 8,
                        "shards": 2,
                        "concurrency": 2,
                        "time_control": "1+0",
                        "threads": 1,
                        "hash_mb": 16,
                        "openings": "book.epd",
                        "opening_sha256": "a" * 64,
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
            )

            payload = json.loads(output.read_text(encoding="utf-8"))
            self.assertEqual(matrix, {"shard": [0, 1, 2]})
            self.assertEqual(payload["candidate_ref"], "feature")
            self.assertEqual(payload["games"], 12)
            self.assertEqual(payload["time_control"], "0.5+0.01")


if __name__ == "__main__":
    unittest.main()
