import json
from pathlib import Path
import tempfile
import unittest

from tools.experiment.manifest import (
    ArtifactIdentity,
    ExperimentManifest,
    sha256_file,
)


class ManifestTests(unittest.TestCase):
    def test_artifact_identity_detects_content_change(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            artifact = Path(directory) / "engine.exe"
            artifact.write_bytes(b"candidate-v1")
            identity = ArtifactIdentity.from_path(artifact)
            identity.verify()

            artifact.write_bytes(b"candidate-v2")

            with self.assertRaisesRegex(ValueError, "SHA-256 mismatch"):
                identity.verify()

    def test_manifest_round_trips_all_artifact_hashes(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            candidate = root / "candidate.exe"
            opponent = root / "opponent.exe"
            openings = root / "openings.epd"
            runner = root / "runner.exe"
            candidate.write_bytes(b"candidate")
            opponent.write_bytes(b"opponent")
            openings.write_text("8/8/8/8/8/8/4K3/7k w - - 0 1\n", encoding="utf-8")
            runner.write_bytes(b"runner")

            manifest = ExperimentManifest.create(
                experiment_id="unit-test",
                source_commit="0123456789abcdef",
                candidate=ArtifactIdentity.from_path(candidate),
                opponent=ArtifactIdentity.from_path(opponent),
                openings=ArtifactIdentity.from_path(openings),
                runner=ArtifactIdentity.from_path(runner),
                configuration={"games": 2, "repeat": True},
            )
            encoded = json.loads(manifest.to_json())

            self.assertEqual(encoded["candidate"]["sha256"], sha256_file(candidate))
            self.assertEqual(encoded["opponent"]["sha256"], sha256_file(opponent))
            self.assertEqual(encoded["openings"]["sha256"], sha256_file(openings))
            self.assertEqual(encoded["runner"]["sha256"], sha256_file(runner))
            self.assertEqual(encoded["configuration"]["games"], 2)
            self.assertEqual(encoded["source_commit"], "0123456789abcdef")


if __name__ == "__main__":
    unittest.main()
