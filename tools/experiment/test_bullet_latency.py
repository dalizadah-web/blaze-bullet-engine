import unittest
from pathlib import Path
import subprocess
import sys

from tools.bullet_latency import TimeControl, percentile, uci_go_command


class TimeControlTests(unittest.TestCase):
    def test_direct_script_launch_resolves_project_imports(self) -> None:
        completed = subprocess.run(
            [sys.executable, str(Path("tools/bullet_latency.py")), "--help"],
            capture_output=True,
            text=True,
            check=False,
        )
        self.assertEqual(completed.returncode, 0, completed.stderr)

    def test_parses_every_supported_bullet_shape(self) -> None:
        expected = {
            "0+1": (0, 1000),
            "0.5+0": (500, 0),
            "1+0": (1000, 0),
            "1+1": (1000, 1000),
            "2+0": (2000, 0),
            "0+2": (0, 2000),
            "2+1": (2000, 1000),
        }
        for text, milliseconds in expected.items():
            with self.subTest(text=text):
                control = TimeControl.parse(text)
                self.assertEqual((control.base_ms, control.increment_ms), milliseconds)

    def test_rejects_negative_and_malformed_controls(self) -> None:
        for text in ("", "1", "-1+0", "1+-2", "fast+1"):
            with self.subTest(text=text):
                with self.assertRaises(ValueError):
                    TimeControl.parse(text)

    def test_pure_increment_go_command_preserves_zero_clock(self) -> None:
        command = uci_go_command(TimeControl.parse("0+1"), white_to_move=True)
        self.assertEqual(command, "go wtime 0 btime 0 winc 1000 binc 1000")


class PercentileTests(unittest.TestCase):
    def test_uses_nearest_rank_for_tail_latency(self) -> None:
        self.assertEqual(percentile([1.0, 2.0, 3.0, 100.0], 0.99), 100.0)
        self.assertEqual(percentile([1.0, 2.0, 3.0, 100.0], 0.50), 2.0)

    def test_rejects_empty_samples(self) -> None:
        with self.assertRaises(ValueError):
            percentile([], 0.99)


if __name__ == "__main__":
    unittest.main()
