import unittest
from unittest.mock import patch

from tools.cloud_match.combine import combine_lanes


def lane(name: str, wins: int, losses: int, *, quarantined_pairs: int = 0) -> dict:
    clean_pairs = wins + losses
    clean_games = clean_pairs * 2
    quarantined_games = quarantined_pairs * 2
    return {
        "schema_version": 3,
        "lane": name,
        "expected_games": clean_games + quarantined_games,
        "completed_games": clean_games + quarantined_games,
        "clean_games": clean_games,
        "clean_pairs": clean_pairs,
        "quarantined_games": quarantined_games,
        "quarantined_pairs": quarantined_pairs,
        "raw_wdl": {"wins": wins * 2, "draws": quarantined_pairs, "losses": losses * 2 + quarantined_pairs},
        "clean_wdl": {"wins": wins * 2, "draws": 0, "losses": losses * 2},
        "counts": {
            "wins2": wins,
            "wins1_draw1": 0,
            "draws2": 0,
            "losses1_draw1": 0,
            "losses2": losses,
        },
        "termination_counts": {
            "clean": {"ordinary": clean_games, "adjudication": 0},
            "candidate": {"time_loss": quarantined_pairs, "illegal_move": 0, "disconnect": 0, "stall": 0},
            "opponent": {"time_loss": 0, "illegal_move": 0, "disconnect": 0, "stall": 0},
            "infrastructure_unknown": {"unterminated": 0, "malformed": 0, "unknown": 0, "contradictory": 0, "runner_failure": 0, "paired_quarantine": quarantined_pairs},
        },
        "abnormal_games": [
            record
            for index in range(quarantined_pairs)
            for record in ({
                "game_id": f"{name}-p{index:06d}-w",
                "round": str(clean_games + index + 1),
                "result": "0-1",
                "candidate_color": "white",
                "termination": "time forfeit",
                "category": "time_loss",
                "offender": "candidate",
                "reason": "engine failure",
            }, {
                "game_id": f"{name}-p{index:06d}-b",
                "round": str(clean_games + index + 2),
                "result": "1/2-1/2",
                "candidate_color": "black",
                "termination": "",
                "category": "paired_quarantine",
                "offender": "unknown",
                "reason": "color-paired game was quarantined",
            })
        ],
        "configuration": {
            "candidate_ref": "candidate",
            "baseline_ref": "baseline",
            "time_control": "0.5+0",
            "threads": 1,
            "hash_mb": 16,
            "opening_sha256": "a" * 64,
            "sprt": {"elo0": 0.0, "elo1": 5.0, "alpha": 0.05, "beta": 0.05},
        },
        "artifacts": {"candidate_sha256": name * 64},
    }


class CombineLanesTests(unittest.TestCase):
    def test_combines_cross_platform_lane_evidence(self) -> None:
        result = combine_lanes([lane("c", 2, 0), lane("l", 1, 1)])
        self.assertEqual(result["expected_games"], 8)
        self.assertEqual(result["counts"]["wins2"], 3)
        self.assertEqual(result["counts"]["losses2"], 1)
        self.assertEqual(len(result["lanes"]), 2)

    def test_preserves_quarantined_pairs_and_lane_strata(self) -> None:
        cloud = lane("c", 1, 0, quarantined_pairs=1)
        local = lane("l", 0, 1, quarantined_pairs=2)

        result = combine_lanes([cloud, local])

        self.assertEqual(result["expected_games"], 10)
        self.assertEqual(result["clean_pairs"], 2)
        self.assertEqual(result["quarantined_pairs"], 3)
        self.assertEqual(result["lanes"][0]["quarantined_pairs"], 1)
        self.assertEqual(result["lanes"][1]["quarantined_pairs"], 2)
        self.assertEqual(len(result["lanes"][1]["abnormal_games"]), 4)

    def test_rejects_backward_incompatible_lane_schema(self) -> None:
        cloud = lane("c", 1, 0)
        local = lane("l", 1, 0)
        local["schema_version"] = 2

        with self.assertRaisesRegex(ValueError, "unsupported lane schema"):
            combine_lanes([cloud, local])

    def test_rejects_forged_lane_wdl_evidence(self) -> None:
        cloud = lane("c", 1, 0)
        local = lane("l", 1, 0)
        local["clean_wdl"] = {"wins": 1, "draws": 1, "losses": 0}

        with self.assertRaisesRegex(ValueError, "clean W/D/L contradicts"):
            combine_lanes([cloud, local])

        local = lane("l", 1, 0, quarantined_pairs=1)
        local["raw_wdl"] = {"wins": 2, "draws": 2, "losses": 0}
        with self.assertRaisesRegex(ValueError, "raw W/D/L does not reconcile"):
            combine_lanes([cloud, local])

    def test_zero_clean_lanes_never_call_sprt(self) -> None:
        cloud = lane("c", 0, 0, quarantined_pairs=1)
        local = lane("l", 0, 0, quarantined_pairs=1)

        with patch("tools.cloud_match.combine.sprt_llr", side_effect=AssertionError("SPRT called")), patch(
            "tools.cloud_match.combine.sprt_decision", side_effect=AssertionError("SPRT called")
        ):
            result = combine_lanes([cloud, local])

        self.assertEqual(result["decision"], "no_clean_pairs")
        self.assertEqual(result["llr"], 0.0)

    def test_rejects_incompatible_time_controls(self) -> None:
        cloud = lane("c", 1, 0)
        local = lane("l", 1, 0)
        local["configuration"]["time_control"] = "1+0"
        with self.assertRaisesRegex(ValueError, "incompatible lane configuration"):
            combine_lanes([cloud, local])

    def test_rejects_incompatible_sprt_parameters(self) -> None:
        cloud = lane("c", 1, 0)
        local = lane("l", 1, 0)
        local["configuration"]["sprt"] = {"elo0": 0.0, "elo1": 10.0, "alpha": 0.05, "beta": 0.05}
        with self.assertRaisesRegex(ValueError, "incompatible lane configuration"):
            combine_lanes([cloud, local])


if __name__ == "__main__":
    unittest.main()
