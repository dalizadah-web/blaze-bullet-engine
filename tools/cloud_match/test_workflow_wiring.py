"""Regression checks for the immutable-spec handoff in the live workflow."""

from __future__ import annotations

from pathlib import Path
import unittest


WORKFLOW = Path(__file__).resolve().parents[2] / ".github" / "workflows" / "cloud-match.yml"
HYBRID = Path(__file__).resolve().parents[1] / "hybrid_match.ps1"
CLOUD = Path(__file__).resolve().parents[1] / "cloud_match.ps1"
SPSA = Path(__file__).resolve().parents[2] / ".github" / "workflows" / "spsa-tune.yml"
QUALIFICATION = Path(__file__).resolve().parents[2] / ".github" / "workflows" / "spsa-qualify.yml"


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
        self.assertIn("opening_suite_positions:", workflow)
        self.assertIn("--opening-suite-positions '${{ inputs.opening_suite_positions }}'", workflow)

    def test_cloud_launcher_defaults_to_10k_full_suite_cycles(self) -> None:
        hybrid = HYBRID.read_text(encoding="utf-8")
        cloud = CLOUD.read_text(encoding="utf-8")

        self.assertIn("[int]$Games = 10000", cloud)
        self.assertIn("[int]$Shards = 40", cloud)
        self.assertIn("[int]$OpeningStart = 1", cloud)
        self.assertIn("[int]$OpeningRepeats = 10", cloud)
        self.assertIn('"opening_start=$OpeningStart"', cloud)
        self.assertIn('"opening_repeats=$OpeningRepeats"', cloud)
        self.assertIn('"concurrency=$Concurrency"', cloud)
        self.assertIn("default-match.json", cloud)
        self.assertIn("OpeningSuitePositions", cloud)
        self.assertNotIn("500 * $OpeningRepeats", cloud)

    def test_hybrid_launcher_is_quarantined_until_disjoint_lane_support_exists(self) -> None:
        hybrid = HYBRID.read_text(encoding="utf-8")
        self.assertIn("Hybrid local+cloud lanes are quarantined", hybrid)
        self.assertIn("-CloudOnly", hybrid)

    def test_spsa_uses_safe_same_binary_cloud_plumbing(self) -> None:
        workflow = WORKFLOW.read_text(encoding="utf-8")
        self.assertIn("candidate_initstr:", workflow)
        self.assertIn("CANDIDATE_INITSTR: ${{ inputs.candidate_initstr }}", workflow)
        self.assertNotIn("CANDIDATE_INITSTR=${{ inputs.candidate_initstr }}", workflow)
        self.assertIn("cp bundle/candidate bundle/baseline", workflow)
        self.assertIn("same-commit SPSA engines are not byte-identical", workflow)
        self.assertIn("TIME_CONTROL: ${{ inputs.time_control }}", workflow)
        self.assertIn('--time-control "$TIME_CONTROL"', workflow)
        self.assertNotIn("--time-control '${{ inputs.time_control }}'", workflow)

    def test_spsa_controller_is_sequential_resumable_and_fixed_to_cloud_geometry(self) -> None:
        workflow = SPSA.read_text(encoding="utf-8")
        self.assertIn("actions: write", workflow)
        self.assertIn("group: spsa-controller-${{ inputs.campaign_id }}", workflow)
        self.assertIn("state_run_id", workflow)
        self.assertNotIn("gh run watch", workflow)
        self.assertIn("spsa-state-${{ inputs.campaign_id }}", workflow)
        self.assertIn("Continue campaign after state is durable", workflow)
        self.assertIn('test "$CONTROLLER_REF_TYPE" = tag', workflow)
        self.assertIn("Upload durable dispatch receipt", workflow)
        self.assertIn("campaign/evidence", workflow)
        self.assertNotIn("plan.dispatched.json", workflow)
        self.assertNotIn("campaign/plan.dispatched", workflow)
        self.assertNotIn("$sha)])\"", workflow)
        self.assertIn('--arg state_run_id "$GITHUB_RUN_ID"', workflow)
        self.assertIn('-f "callback_payload=$callback_payload"', workflow)
        self.assertIn("controller_action=tune", workflow)
        self.assertIn("Upload resumable state while child runs asynchronously", workflow)
        config = (Path(__file__).resolve().parents[2] / "config" / "spsa" / "search-v3.json").read_text(encoding="utf-8")
        self.assertIn('"shards": 40', config)
        self.assertIn('"concurrency": 2', config)
        self.assertIn('"time_control": "1+0"', config)

    def test_spsa_qualification_is_resumable_and_sealed(self) -> None:
        workflow = QUALIFICATION.read_text(encoding="utf-8")
        self.assertIn('test "$CONTROLLER_REF_TYPE" = tag', workflow)
        self.assertIn("qualification-dispatch.json", workflow)
        self.assertIn("multiple qualification runs exist", workflow)
        self.assertIn("tools.cloud_match.spsa qualify", workflow)
        self.assertIn("qualification-summary.json", workflow)
        self.assertNotIn("$sha)])\"", workflow)
        self.assertIn("--arg workflow cloud-match.yml --arg action qualify", workflow)

    def test_cloud_shards_use_a_measured_eighty_game_barrier_and_callback(self) -> None:
        workflow = WORKFLOW.read_text(encoding="utf-8")
        self.assertIn("shard-start-epoch.txt", workflow)
        self.assertIn("Synchronize shard start without API polling", workflow)
        self.assertNotIn("actions/runs/$GITHUB_RUN_ID/artifacts", workflow)
        self.assertIn("Resume SPSA controller after evidence is durable", workflow)
        self.assertIn('--source-run-attempt "$GITHUB_RUN_ATTEMPT"', workflow)


if __name__ == "__main__":
    unittest.main()
