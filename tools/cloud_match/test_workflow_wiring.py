"""Regression checks for the immutable-spec handoff in the live workflow."""

from __future__ import annotations

from pathlib import Path
import unittest


WORKFLOW = Path(__file__).resolve().parents[2] / ".github" / "workflows" / "cloud-match.yml"


class CloudWorkflowWiringTests(unittest.TestCase):
    def test_uses_one_finalized_spec_from_prepare_through_aggregate(self) -> None:
        workflow = WORKFLOW.read_text(encoding="utf-8")

        self.assertIn("fetch-depth: 0", workflow)
        self.assertIn("--repo-root .", workflow)
        self.assertIn("finalize_frozen_spec", workflow)
        self.assertIn("finalized-match-spec-${{ needs.prepare.outputs.experiment_id }}", workflow)
        self.assertIn("--spec finalized/spec.json", workflow)


if __name__ == "__main__":
    unittest.main()
