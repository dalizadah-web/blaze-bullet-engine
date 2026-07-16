import json
from pathlib import Path
import subprocess
import tempfile
import textwrap
import unittest

from tools.experiment.match import (
    MatchSpec,
    SprtSpec,
    build_runner_command,
    parse_paired_pgn,
    run_match,
)
from tools.experiment.manifest import sha256_file


class MatchSpecTests(unittest.TestCase):
    def test_rejects_an_odd_game_count(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            config = Path(directory) / "match.json"
            config.write_text(
                json.dumps(
                    {
                        "schema_version": 1,
                        "name": "invalid",
                        "games": 3,
                        "concurrency": 1,
                        "time_control": "10+0.1",
                        "threads": 1,
                        "hash_mb": 16,
                        "repeat": True,
                        "opening_format": "epd",
                        "openings": "openings.epd",
                        "opening_sha256": "0" * 64,
                        "opponent_sha256": None,
                        "sprt": {
                            "elo0": 0.0,
                            "elo1": 5.0,
                            "alpha": 0.05,
                            "beta": 0.05,
                        },
                    }
                ),
                encoding="utf-8",
            )

            with self.assertRaisesRegex(ValueError, "positive even number"):
                MatchSpec.from_json(config)

    def test_runner_command_pins_pairing_and_engine_resources(self) -> None:
        spec = MatchSpec(
            schema_version=1,
            name="command-test",
            games=2,
            concurrency=1,
            time_control="10+0.1",
            threads=2,
            hash_mb=64,
            repeat=True,
            opening_format="epd",
            openings="openings.epd",
            opening_sha256="0" * 64,
            opponent_sha256=None,
            sprt=SprtSpec(0.0, 5.0, 0.05, 0.05),
        )

        command = build_runner_command(
            spec,
            runner=Path("cutechess-cli.exe"),
            candidate=Path("blaze.exe"),
            opponent=Path("opponent.exe"),
            openings=Path("openings.epd"),
            pgn=Path("games.pgn"),
            games=2,
            candidate_name="Blaze",
            opponent_name="Opponent",
        )

        self.assertIn("option.Threads=2", command)
        self.assertIn("option.Hash=64", command)
        self.assertIn("tc=10+0.1", command)
        self.assertIn("-repeat", command)
        self.assertEqual(command[command.index("-games") + 1], "2")


class PgnPairTests(unittest.TestCase):
    def test_maps_color_swapped_wins_to_two_win_category(self) -> None:
        pgn = textwrap.dedent(
            """
            [Event "paired"]
            [Round "1"]
            [White "Blaze"]
            [Black "Opponent"]
            [Result "1-0"]

            1. e4 e5 1-0

            [Event "paired"]
            [Round "2"]
            [White "Opponent"]
            [Black "Blaze"]
            [Result "0-1"]

            1. e4 e5 0-1
            """
        ).strip()
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "games.pgn"
            path.write_text(pgn + "\n", encoding="utf-8")

            result = parse_paired_pgn(path, "Blaze", "Opponent", expected_games=2)

        self.assertEqual(result.as_tuple(), (1, 0, 0, 0, 0))

    def test_rejects_pair_without_color_reversal(self) -> None:
        pgn = textwrap.dedent(
            """
            [Event "paired"]
            [Round "1"]
            [White "Blaze"]
            [Black "Opponent"]
            [Result "1/2-1/2"]

            1. e4 e5 1/2-1/2

            [Event "paired"]
            [Round "2"]
            [White "Blaze"]
            [Black "Opponent"]
            [Result "1/2-1/2"]

            1. e4 e5 1/2-1/2
            """
        ).strip()
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "games.pgn"
            path.write_text(pgn + "\n", encoding="utf-8")

            with self.assertRaisesRegex(ValueError, "swap colors"):
                parse_paired_pgn(path, "Blaze", "Opponent", expected_games=2)


class MatchExecutionTests(unittest.TestCase):
    def test_run_match_writes_hashed_manifest_and_paired_result(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            candidate = root / "blaze.exe"
            opponent = root / "opponent.exe"
            runner = root / "runner.exe"
            openings = root / "openings.epd"
            for path, content in (
                (candidate, b"candidate"),
                (opponent, b"opponent"),
                (runner, b"runner"),
            ):
                path.write_bytes(content)
            openings.write_text(
                'rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - id "start";\n',
                encoding="utf-8",
            )
            spec = MatchSpec(
                schema_version=1,
                name="execution-test",
                games=2,
                concurrency=1,
                time_control="1+0.01",
                threads=1,
                hash_mb=16,
                repeat=True,
                opening_format="epd",
                openings=str(openings),
                opening_sha256=sha256_file(openings),
                opponent_sha256=None,
                sprt=SprtSpec(0.0, 5.0, 0.05, 0.05),
            )

            def fake_executor(command: list[str], **_: object) -> subprocess.CompletedProcess[str]:
                pgn_path = Path(command[command.index("-pgnout") + 1])
                pgn_path.write_text(
                    textwrap.dedent(
                        """
                        [Event "paired"]
                        [Round "1"]
                        [White "Blaze"]
                        [Black "Opponent"]
                        [Result "1/2-1/2"]

                        1. e4 e5 1/2-1/2

                        [Event "paired"]
                        [Round "2"]
                        [White "Opponent"]
                        [Black "Blaze"]
                        [Result "1/2-1/2"]

                        1. e4 e5 1/2-1/2
                        """
                    ).strip()
                    + "\n",
                    encoding="utf-8",
                )
                return subprocess.CompletedProcess(command, 0, stdout="Finished match\n")

            output = root / "result"
            result = run_match(
                spec,
                candidate=candidate,
                opponent=opponent,
                runner=runner,
                output=output,
                source_commit="0123456789abcdef",
                candidate_name="Blaze",
                opponent_name="Opponent",
                executor=fake_executor,
            )

            self.assertEqual(result.counts.as_tuple(), (0, 0, 1, 0, 0))
            self.assertTrue((output / "manifest.json").is_file())
            self.assertTrue((output / "result.json").is_file())
            self.assertEqual(
                json.loads((output / "manifest.json").read_text(encoding="utf-8"))["runner"][
                    "sha256"
                ],
                sha256_file(runner),
            )


if __name__ == "__main__":
    unittest.main()
