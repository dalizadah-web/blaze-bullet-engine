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
        self.assertIn("default: 1", workflow)
        self.assertIn("opening_repeats:", workflow)
        self.assertIn("default: 10", workflow)
        self.assertIn("--opening-start '${{ inputs.opening_start }}'", workflow)

    def test_cloud_launcher_defaults_to_10k_full_suite_cycles(self) -> None:
        hybrid = HYBRID.read_text(encoding="utf-8")
        cloud = CLOUD.read_text(encoding="utf-8")

        self.assertIn("[int]$Games = 10000", cloud)
        self.assertIn("[int]$Shards = 20", cloud)
        self.assertIn("[int]$OpeningStart = 1", cloud)
        self.assertIn("[int]$OpeningRepeats = 10", cloud)
        self.assertIn('"opening_start=$OpeningStart"', cloud)
        self.assertIn('"opening_repeats=$OpeningRepeats"', cloud)
        self.assertIn("default-match.json", cloud)
        self.assertIn("OpeningSuitePositions", cloud)
        self.assertNotIn("500 * $OpeningRepeats", cloud)

    def test_hybrid_launcher_is_quarantined_until_disjoint_lane_support_exists(self) -> None:
        hybrid = HYBRID.read_text(encoding="utf-8")
        self.assertIn("Hybrid local+cloud lanes are quarantined", hybrid)
        self.assertIn("-CloudOnly", hybrid)


if __name__ == "__main__":
    unittest.main()
