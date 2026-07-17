from pathlib import Path
import tempfile
import unittest

from tools.cloud_match.worker import run_worker, write_shard_openings


class WorkerOpeningTests(unittest.TestCase):
    def test_selects_deterministic_cyclic_openings_for_shard(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "openings.epd"
            source.write_text("fen-0\n\nfen-1\nfen-2\n", encoding="utf-8")
            destination = root / "shard.epd"

            selected = write_shard_openings(source, [1, 3, 5], destination)

            self.assertEqual(selected, ["fen-1", "fen-0", "fen-2"])
            self.assertEqual(destination.read_text(encoding="utf-8"), "fen-1\nfen-0\nfen-2\n")

    def test_rejects_empty_opening_source(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "openings.epd"
            source.write_text("\n", encoding="utf-8")

            with self.assertRaisesRegex(ValueError, "no opening positions"):
                write_shard_openings(source, [0], root / "shard.epd")

    def test_rejects_a_spec_without_frozen_binary_hashes(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            spec = root / "spec.json"
            spec.write_text(
                """{
  \"schema_version\": 1,
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
