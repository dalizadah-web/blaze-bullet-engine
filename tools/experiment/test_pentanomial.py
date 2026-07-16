import unittest

from tools.experiment.pentanomial import (
    Pentanomial,
    sprt_decision,
    sprt_llr,
)


class PentanomialTests(unittest.TestCase):
    def test_color_swapped_pair_mapping(self) -> None:
        counts = Pentanomial.from_pair_scores(
            [(1.0, 1.0), (1.0, 0.5), (0.5, 0.5), (0.0, 0.5), (0.0, 0.0)]
        )

        self.assertEqual(counts.as_tuple(), (1, 1, 1, 1, 1))

    def test_rejects_incomplete_pair_scores(self) -> None:
        with self.assertRaisesRegex(ValueError, "two candidate scores"):
            Pentanomial.from_pair_scores([(1.0,)])

    def test_rejects_invalid_game_score(self) -> None:
        with self.assertRaisesRegex(ValueError, "0, 0.5, or 1"):
            Pentanomial.from_pair_scores([(1.0, 0.25)])

    def test_symmetric_results_support_zero_elo(self) -> None:
        counts = Pentanomial(20, 40, 80, 40, 20)

        self.assertLess(sprt_llr(counts, 0.0, 5.0), 0.0)

    def test_decisive_positive_pairs_accept(self) -> None:
        decision = sprt_decision(
            Pentanomial(500, 100, 20, 0, 0),
            elo0=0.0,
            elo1=5.0,
            alpha=0.05,
            beta=0.05,
        )

        self.assertEqual(decision, "accept")

    def test_decisive_negative_pairs_reject(self) -> None:
        decision = sprt_decision(
            Pentanomial(0, 0, 20, 100, 500),
            elo0=0.0,
            elo1=5.0,
            alpha=0.05,
            beta=0.05,
        )

        self.assertEqual(decision, "reject")


if __name__ == "__main__":
    unittest.main()
