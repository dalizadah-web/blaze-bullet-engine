import unittest

from tools.cloud_match.combine import combine_lanes


def lane(name: str, wins: int, losses: int, *, quarantined_pairs: int = 0) -> dict:
    clean_pairs = wins + losses
    clean_games = clean_pairs * 2
    quarantined_games = quarantined_pairs * 2
    return {
        "schema_version": 2,
        "lane": name,
        "expected_games": clean_games + quarantined_games,
        "completed_games": clean_games + quarantined_games,
        "clean_games": clean_games,
        "clean_pairs": clean_pairs,
        "quarantined_games": quarantined_games,
        "quarantined_pairs": quarantined_pairs,
        "raw_wdl": {"wins": wins * 2, "draws": quarantined_games, "losses": losses * 2},
        "counts": {
            "wins2": wins,
            "wins1_draw1": 0,
            "draws2": 0,
            "losses1_draw1": 0,
            "losses2": losses,
        },
        "termination_counts": {
            "clean": {"ordinary": clean_games + quarantined_pairs, "adjudication": 0},
            "candidate": {"time_loss": quarantined_pairs, "illegal_move": 0, "disconnect": 0, "stall": 0},
            "opponent": {"time_loss": 0, "illegal_move": 0, "disconnect": 0, "stall": 0},
            "infrastructure_unknown": {"unterminated": 0, "malformed": 0, "unknown": 0, "contradictory": 0, "runner_failure": 0},
        },
        "abnormal_games": [
            {
                "game_id": f"{name}-p{index:06d}-w",
                "round": str(clean_games + index + 1),
                "result": "0-1",
                "candidate_color": "white",
                "termination": "time forfeit",
                "category": "time_loss",
                "offender": "candidate",
                "reason": "engine failure",
            }
            for index in range(quarantined_pairs)
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
        self.assertEqual(len(result["lanes"][1]["abnormal_games"]), 2)

    def test_rejects_backward_incompatible_lane_schema(self) -> None:
        cloud = lane("c", 1, 0)
        local = lane("l", 1, 0)
        local["schema_version"] = 1

        with self.assertRaisesRegex(ValueError, "unsupported lane schema"):
            combine_lanes([cloud, local])

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
