from pathlib import Path
import tempfile
import unittest

from tools.cloud_match.worker import write_shard_openings


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


if __name__ == "__main__":
    unittest.main()
