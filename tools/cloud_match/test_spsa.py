from __future__ import annotations

import copy
import hashlib
import json
import unittest

from tools.cloud_match.spsa import (
    _final_document,
    apply_trial,
    final_vector,
    initialize_state,
    plan_trial,
    qualification_report,
    validate_config,
)
from tools.experiment.pentanomial import Pentanomial, sprt_decision, sprt_llr


COMMIT = "a" * 40
CONTROLLER_COMMIT = "d" * 40
SHA256 = "e" * 64
RUN_ID = 12345


def config() -> dict[str, object]:
    return {
        "schema_version": 1,
        "campaign_id": "test-campaign",
        "master_seed": "12" * 32,
        "iterations": 4,
        "gain": {"a0": 0.01, "c0": 0.1, "alpha": 0.602, "gamma": 0.101, "A": 1.0},
        "match": {
            "games": 80,
            "shards": 40,
            "concurrency": 2,
            "threads": 1,
            "hash_mb": 16,
            "time_control": "1+0",
            "required_peak_concurrent_games": 80,
            "openings_per_iteration": 40,
            "training_positions": 160,
            "base_config": "config/cloud/train.json",
            "base_config_sha256": SHA256,
            "name": "test-training",
            "openings": "testdata/openings/train.epd",
            "opening_sha256": "f" * 64,
            "sprt": {"elo0": 0.0, "elo1": 5.0, "alpha": 0.05, "beta": 0.05},
        },
        "qualification": {
            "games": 80,
            "opening_positions": 40,
            "shards": 40,
            "concurrency": 2,
            "threads": 1,
            "time_control": "1+0",
            "hash_mb": 16,
            "required_peak_concurrent_games": 80,
            "base_config": "config/cloud/qualification.json",
            "base_config_sha256": SHA256,
            "name": "test-qualification",
            "openings": "testdata/openings/qualification.epd",
            "opening_sha256": "9" * 64,
            "sprt": {"elo0": 0.0, "elo1": 5.0, "alpha": 0.05, "beta": 0.05},
        },
        "parameters": [
            {"name": "rfp_base", "default": 60, "min": 20, "max": 100, "step": 1},
            {"name": "razor_base", "default": 180, "min": 80, "max": 280, "step": 5},
        ],
    }


def summary(
    cfg: dict[str, object],
    counts: dict[str, int],
    *,
    candidate_initstr: str,
    baseline_initstr: str,
    opening_start: int,
    qualification: bool = False,
) -> dict[str, object]:
    protocol = cfg["qualification" if qualification else "match"]
    games = protocol["games"]
    positions = protocol["opening_positions" if qualification else "openings_per_iteration"]
    pentanomial = Pentanomial(*(counts[key] for key in (
        "wins2", "wins1_draw1", "draws2", "losses1_draw1", "losses2"
    )))
    wins = counts["wins2"] * 2 + counts["wins1_draw1"]
    draws = counts["wins1_draw1"] + counts["draws2"] * 2 + counts["losses1_draw1"]
    losses = counts["losses2"] * 2 + counts["losses1_draw1"]
    wdl = {"wins": wins, "draws": draws, "losses": losses}
    spec = {
            "schema_version": 2,
            "name": protocol["name"],
            "candidate_ref": COMMIT,
            "baseline_ref": COMMIT,
            "candidate_commit": COMMIT,
            "baseline_commit": COMMIT,
            "candidate_sha256": "b" * 64,
            "baseline_sha256": "b" * 64,
            "candidate_initstr": candidate_initstr,
            "baseline_initstr": baseline_initstr,
            "games": games,
            "shards": 40,
            "concurrency": 2,
            "time_control": "1+0",
            "threads": 1,
            "hash_mb": 16,
            "openings": protocol["openings"],
            "opening_sha256": protocol["opening_sha256"],
            "opening_start": opening_start,
            "opening_suite_positions": positions,
            "opening_repeats": 1,
            "sprt": protocol["sprt"],
    }
    canonical = json.dumps(spec, sort_keys=True, separators=(",", ":")).encode()
    llr = sprt_llr(pentanomial, protocol["sprt"]["elo0"], protocol["sprt"]["elo1"])
    decision = sprt_decision(
        pentanomial,
        protocol["sprt"]["elo0"],
        protocol["sprt"]["elo1"],
        protocol["sprt"]["alpha"],
        protocol["sprt"]["beta"],
    )
    return {
        "schema_version": 3,
        "source_run_id": RUN_ID,
        "source_run_attempt": 1,
        "lane": "cloud-linux-github-hosted",
        "experiment_id": hashlib.sha256(canonical).hexdigest()[:24],
        "spec": spec | {"opening_count": positions},
        "artifacts": {
            "candidate_commit": COMMIT,
            "baseline_commit": COMMIT,
            "candidate_sha256": "b" * 64,
            "baseline_sha256": "b" * 64,
            "openings_sha256": protocol["opening_sha256"],
            "runner_sha256": "c" * 64,
        },
        "expected_games": games,
        "completed_games": games,
        "clean_games": games,
        "clean_pairs": games // 2,
        "quarantined_games": 0,
        "quarantined_pairs": 0,
        "abnormal_games": [],
        "raw_wdl": wdl,
        "clean_wdl": wdl,
        "counts": counts,
        "termination_counts": {
            "clean": {"ordinary": games, "adjudication": 0},
            "candidate": {"time_loss": 0, "illegal_move": 0, "disconnect": 0, "stall": 0},
            "opponent": {"time_loss": 0, "illegal_move": 0, "disconnect": 0, "stall": 0},
            "infrastructure_unknown": {
                "unterminated": 0, "malformed": 0, "unknown": 0,
                "contradictory": 0, "runner_failure": 0, "paired_quarantine": 0,
            },
        },
        "shards": 40,
        "peak_concurrent_games": 80,
        "llr": llr,
        "decision": decision,
    }


def dispatch(plan: dict[str, object]) -> dict[str, object]:
    return {
        "schema_version": 1,
        "trial_id": plan["trial_id"],
        "iteration": plan["iteration"],
        "child_run_id": RUN_ID,
        "child_run_attempt": 1,
        "child_run_url": f"https://github.com/example/repo/actions/runs/{RUN_ID}",
        "controller_commit": CONTROLLER_COMMIT,
    }


class SpsaTests(unittest.TestCase):
    def test_plan_is_reproducible_and_uses_distinct_vectors(self) -> None:
        cfg = config()
        state = initialize_state(cfg, COMMIT, CONTROLLER_COMMIT)
        first = plan_trial(cfg, state)
        second = plan_trial(copy.deepcopy(cfg), copy.deepcopy(state))
        self.assertEqual(first, second)
        self.assertNotEqual(first["plus"], first["minus"])
        self.assertEqual(first["opening_start"], 1)

    def test_plan_honors_configured_training_opening_offset(self) -> None:
        cfg = config()
        cfg["match"]["opening_start"] = 81
        state = initialize_state(cfg, COMMIT, CONTROLLER_COMMIT)
        self.assertEqual(plan_trial(cfg, state)["opening_start"], 81)

    def test_candidate_role_is_normalized_before_update(self) -> None:
        cfg = config()
        state = initialize_state(cfg, COMMIT, CONTROLLER_COMMIT)
        plan = plan_trial(cfg, state)
        plus_wins = {
            "wins2": 30,
            "wins1_draw1": 0,
            "draws2": 10,
            "losses1_draw1": 0,
            "losses2": 0,
        }
        candidate_counts = plus_wins if plan["candidate_is_plus"] else {
            "wins2": 0,
            "wins1_draw1": 0,
            "draws2": 10,
            "losses1_draw1": 0,
            "losses2": 30,
        }
        evidence = summary(
            cfg,
            candidate_counts,
            candidate_initstr=plan["candidate_initstr"],
            baseline_initstr=plan["baseline_initstr"],
            opening_start=plan["opening_start"],
        )
        updated, record = apply_trial(cfg, state, plan, dispatch(plan), evidence)
        self.assertEqual(updated["next_iteration"], 1)
        self.assertGreater(float.fromhex(record["direction_hex"]), 0)

    def test_incomplete_or_nonidentical_evidence_is_rejected(self) -> None:
        cfg = config()
        state = initialize_state(cfg, COMMIT, CONTROLLER_COMMIT)
        plan = plan_trial(cfg, state)
        evidence = summary(cfg, {
            "wins2": 0,
            "wins1_draw1": 0,
            "draws2": 40,
            "losses1_draw1": 0,
            "losses2": 0,
        }, candidate_initstr=plan["candidate_initstr"], baseline_initstr=plan["baseline_initstr"], opening_start=plan["opening_start"])
        evidence["artifacts"]["baseline_sha256"] = "c" * 64
        with self.assertRaisesRegex(ValueError, "artifact mismatch"):
            apply_trial(cfg, state, plan, dispatch(plan), evidence)

    def test_tail_average_not_best_trial_selects_final_vector(self) -> None:
        cfg = config()
        state = initialize_state(cfg, COMMIT, CONTROLLER_COMMIT)
        state["next_iteration"] = 4
        state["status"] = "training-complete"
        state["applied_trials"] = [f"{index:024x}" for index in range(4)]
        state["last_trial_sha256"] = "1" * 64
        state["tail_sums_hex"] = [(1.2).hex(), (0.4).hex()]
        state["tail_count"] = 2
        result = final_vector(cfg, state)
        self.assertEqual(result["rfp_base"], 68)
        self.assertEqual(result["razor_base"], 120)

    def test_qualification_recomputes_and_seals_evidence(self) -> None:
        cfg = config()
        state = initialize_state(cfg, COMMIT, CONTROLLER_COMMIT)
        state.update({
            "next_iteration": 4,
            "status": "training-complete",
            "applied_trials": [f"{index:024x}" for index in range(4)],
            "last_trial_sha256": "1" * 64,
            "tail_count": 2,
            "tail_sums_hex": [(1.0).hex(), (1.0).hex()],
        })
        vector = final_vector(cfg, state)
        final = _final_document(cfg, state, vector)
        counts = {"wins2": 0, "wins1_draw1": 0, "draws2": 40, "losses1_draw1": 0, "losses2": 0}
        evidence = summary(
            cfg,
            counts,
            candidate_initstr=final["initstr"],
            baseline_initstr="UseNNUE=true\nEvalFile=nn-c288c895ea92.nnue",
            opening_start=1,
            qualification=True,
        )
        qualification_dispatch = {
            "schema_version": 1,
            "campaign_id": state["campaign_id"],
            "kind": "qualification",
            "child_run_id": RUN_ID,
            "child_run_attempt": 1,
            "controller_commit": CONTROLLER_COMMIT,
        }
        report = qualification_report(cfg, state, final, qualification_dispatch, evidence)
        self.assertEqual(report["sprt_decision"], "continue")
        self.assertFalse(report["accepted"])

        evidence["decision"] = "accept"
        with self.assertRaisesRegex(ValueError, "decision mismatch"):
            qualification_report(cfg, state, final, qualification_dispatch, evidence)

        evidence["decision"] = report["sprt_decision"]
        evidence["source_run_attempt"] = 2
        with self.assertRaisesRegex(ValueError, "source run identity"):
            qualification_report(cfg, state, final, qualification_dispatch, evidence)

    def test_state_is_bound_to_controller_commit(self) -> None:
        cfg = config()
        state = initialize_state(cfg, COMMIT, CONTROLLER_COMMIT)
        state["controller_commit"] = "not-a-commit"
        with self.assertRaisesRegex(ValueError, "controller commit"):
            plan_trial(cfg, state)

    def test_configuration_rejects_insufficient_unique_openings(self) -> None:
        cfg = config()
        cfg["match"]["training_positions"] = 159
        with self.assertRaisesRegex(ValueError, "too small"):
            validate_config(cfg)

    def test_configuration_rejects_degenerate_or_off_grid_ranges(self) -> None:
        cfg = config()
        cfg["parameters"][0]["max"] = cfg["parameters"][0]["min"]
        with self.assertRaisesRegex(ValueError, "bounds or grid"):
            validate_config(cfg)

        cfg = config()
        cfg["parameters"][1]["max"] = 281
        with self.assertRaisesRegex(ValueError, "bounds or grid"):
            validate_config(cfg)


if __name__ == "__main__":
    unittest.main()
