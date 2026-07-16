import unittest

from tools.benchmark import summarize_rates


class BenchmarkSummaryTests(unittest.TestCase):
    def test_reports_median_and_inclusive_interquartile_range(self):
        summary = summarize_rates([100.0, 200.0, 300.0])
        self.assertEqual(summary["median_nps"], 200.0)
        self.assertEqual(summary["q1_nps"], 150.0)
        self.assertEqual(summary["q3_nps"], 250.0)

    def test_rejects_empty_samples(self):
        with self.assertRaises(ValueError):
            summarize_rates([])


if __name__ == "__main__":
    unittest.main()
