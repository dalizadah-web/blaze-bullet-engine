import unittest

from tools.cloud_match.combine import combine_lanes


def lane(name: str, wins: int, losses: int) -> dict:
    return {
        "lane": name,
        "games": (wins + losses) * 2,
        "counts": {
            "wins2": wins,
            "wins1_draw1": 0,
            "draws2": 0,
            "losses1_draw1": 0,
            "losses2": losses,
        },
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
        self.assertEqual(result["games"], 8)
        self.assertEqual(result["counts"]["wins2"], 3)
        self.assertEqual(result["counts"]["losses2"], 1)
        self.assertEqual(len(result["lanes"]), 2)

    def test_rejects_incompatible_time_controls(self) -> None:
        cloud = lane("c", 1, 0)
        local = lane("l", 1, 0)
        local["configuration"]["time_control"] = "1+0"
        with self.assertRaisesRegex(ValueError, "incompatible lane configuration"):
            combine_lanes([cloud, local])


if __name__ == "__main__":
    unittest.main()
