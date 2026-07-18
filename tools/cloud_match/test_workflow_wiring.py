"""Regression checks for the immutable-spec handoff in the live workflow."""

from __future__ import annotations

from pathlib import Path
import unittest


WORKFLOW = Path(__file__).resolve().parents[2] / ".github" / "workflows" / "cloud-match.yml"
HYBRID = Path(__file__).resolve().parents[1] / "hybrid_match.ps1"
CLOUD = Path(__file__).resolve().parents[1] / "cloud_match.ps1"


class CloudWorkflowWiringTests(unittest.TestCase):
    def test_uses_one_finalized_spec_from_prepare_through_aggregate(self) -> None:
        workflow = WORKFLOW.read_text(encoding="utf-8")

        self.assertIn("fetch-depth: 0", workflow)
        self.assertIn("--repo-root .", workflow)
        self.assertIn("finalize_frozen_spec", workflow)
        self.assertIn("finalized-match-spec-${{ needs.prepare.outputs.experiment_id }}", workflow)
        self.assertIn("--spec finalized/spec.json", workflow)

    def test_freezes_cloud_confirmation_lane_start(self) -> None:
        workflow = WORKFLOW.read_text(encoding="utf-8")

        self.assertIn("opening_start:", workflow)
        self.assertIn("default: 251", workflow)
        self.assertIn("--opening-start '${{ inputs.opening_start }}'", workflow)

    def test_local_and_cloud_launchers_freeze_disjoint_250_pair_lanes(self) -> None:
        hybrid = HYBRID.read_text(encoding="utf-8")
        cloud = CLOUD.read_text(encoding="utf-8")

        self.assertIn("[int]$CloudGames = 500", hybrid)
        self.assertIn("[int]$LocalGames = 500", hybrid)
        self.assertIn("[int]$CloudOpeningStart = 251", hybrid)
        self.assertIn("[int]$LocalOpeningStart = 1", hybrid)
        self.assertIn("-OpeningStart $CloudOpeningStart", hybrid)
        self.assertIn("--opening-start $LocalOpeningStart", hybrid)
        self.assertIn("[int]$OpeningStart = 251", cloud)
        self.assertIn('"opening_start=$OpeningStart"', cloud)


if __name__ == "__main__":
    unittest.main()
