import json
from pathlib import Path
import re
import subprocess
import tempfile
import textwrap
import unittest
from unittest.mock import patch

from tools.experiment.match import (
    MatchSpec,
    SprtSpec,
    build_runner_command,
    parse_paired_pgn,
    run_match,
    validate_evidence_payload,
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
    def _parse(self, first_headers: str, second_headers: str):
        first_result = re.search(r'\[Result "([^"]+)"\]', first_headers).group(1)
        second_result = re.search(r'\[Result "([^"]+)"\]', second_headers).group(1)
        pgn = (
            '[Event "paired"]\n[Round "1"]\n[White "Blaze"]\n[Black "Opponent"]\n'
            f"{first_headers}\n\n1. e4 e5 {first_result}\n\n"
            '[Event "paired"]\n[Round "2"]\n[White "Opponent"]\n[Black "Blaze"]\n'
            f"{second_headers}\n\n1. e4 e5 {second_result}"
        )
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "games.pgn"
            path.write_text(pgn + "\n", encoding="utf-8")
            return parse_paired_pgn(path, "Blaze", "Opponent", expected_games=2)

    def test_ordinary_decisive_and_draw_results_remain_clean(self) -> None:
        evidence = self._parse('[Result "1-0"]', '[Result "1/2-1/2"]')

        self.assertEqual(evidence.clean_pairs, 1)
        self.assertEqual(evidence.quarantined_pairs, 0)
        self.assertEqual(evidence.counts.as_tuple(), (0, 1, 0, 0, 0))
        self.assertEqual(evidence.raw_wdl, {"wins": 1, "draws": 1, "losses": 0})
        self.assertEqual(evidence.clean_wdl, {"wins": 1, "draws": 1, "losses": 0})
        self.assertEqual(evidence.termination_counts["clean"], {"ordinary": 2, "adjudication": 0})

    def test_adjudication_is_explicitly_accepted_as_clean(self) -> None:
        evidence = self._parse(
            '[Result "1-0"]\n[Termination "adjudication"]',
            '[Result "0-1"]\n[Termination "adjudication"]',
        )

        self.assertEqual(evidence.clean_pairs, 1)
        self.assertEqual(evidence.termination_counts["clean"], {"ordinary": 0, "adjudication": 2})

    def test_draws2_clean_wdl_disambiguates_two_draws_from_win_loss(self) -> None:
        evidence = self._parse('[Result "1-0"]', '[Result "1-0"]')

        self.assertEqual(evidence.counts.as_tuple(), (0, 0, 1, 0, 0))
        self.assertEqual(evidence.clean_wdl, {"wins": 1, "draws": 0, "losses": 1})
        payload = {"schema_version": 3, **evidence.to_dict()}
        validate_evidence_payload(payload, context="local test")

        payload["clean_wdl"] = {"wins": 0, "draws": 1, "losses": 1}
        with self.assertRaisesRegex(ValueError, "clean W/D/L"):
            validate_evidence_payload(payload, context="local test")

    def test_local_validator_rejects_forged_raw_wdl(self) -> None:
        evidence = self._parse(
            '[Result "0-1"]\n[Termination "time forfeit"]',
            '[Result "0-1"]',
        )
        payload = {"schema_version": 3, **evidence.to_dict()}
        payload["raw_wdl"] = {"wins": 0, "draws": 1, "losses": 1}

        with self.assertRaisesRegex(ValueError, "raw W/D/L does not reconcile"):
            validate_evidence_payload(payload, context="local test")

    def test_time_forfeits_are_attributed_to_the_losing_engine(self) -> None:
        evidence = self._parse(
            '[Result "0-1"]\n[Termination "time forfeit"]',
            '[Result "0-1"]\n[Termination "time forfeit"]',
        )

        self.assertEqual(evidence.clean_pairs, 0)
        self.assertEqual(evidence.quarantined_pairs, 1)
        self.assertEqual(evidence.termination_counts["candidate"]["time_loss"], 1)
        self.assertEqual(evidence.termination_counts["opponent"]["time_loss"], 1)
        self.assertEqual(evidence.raw_wdl, {"wins": 1, "draws": 0, "losses": 1})
        self.assertEqual(evidence.clean_wdl, {"wins": 0, "draws": 0, "losses": 0})
        self.assertEqual(len(evidence.abnormal_games), 2)
        self.assertEqual(evidence.counts.pairs, 0)

    def test_abnormal_termination_categories_are_auditable(self) -> None:
        cases = (
            ("illegal move", "illegal_move", "candidate"),
            ("abandoned", "disconnect", "candidate"),
            ("stalled connection", "stall", "candidate"),
            ("unterminated", "unterminated", "infrastructure_unknown"),
            ("future runner reason", "unknown", "infrastructure_unknown"),
        )
        for termination, category, side in cases:
            with self.subTest(termination=termination):
                evidence = self._parse(
                    f'[Result "0-1"]\n[Termination "{termination}"]',
                    '[Result "0-1"]',
                )
                self.assertEqual(evidence.clean_pairs, 0)
                self.assertEqual(evidence.quarantined_pairs, 1)
                self.assertEqual(len(evidence.abnormal_games), 2)
                self.assertEqual(evidence.abnormal_games[1]["category"], "paired_quarantine")
                self.assertEqual(evidence.termination_counts[side][category], 1)
                self.assertEqual(evidence.abnormal_games[0]["game_id"], "game-000001")
                self.assertEqual(evidence.abnormal_games[0]["termination"], termination)
                self.assertEqual(evidence.abnormal_games[0]["round"], "1")

    def test_contradictory_draw_and_engine_failure_fails_closed(self) -> None:
        evidence = self._parse(
            '[Result "1/2-1/2"]\n[Termination "illegal move"]',
            '[Result "1/2-1/2"]',
        )

        self.assertEqual(evidence.clean_pairs, 0)
        self.assertEqual(evidence.termination_counts["infrastructure_unknown"]["contradictory"], 1)
        self.assertEqual(evidence.abnormal_games[0]["offender"], "unknown")

    def test_uncompleted_result_quarantines_the_whole_pair(self) -> None:
        evidence = self._parse('[Result "*"]', '[Result "1-0"]')

        self.assertEqual(evidence.completed_games, 1)
        self.assertEqual(evidence.quarantined_games, 2)
        self.assertEqual(evidence.clean_games, 0)
        self.assertEqual(evidence.raw_wdl, {"wins": 0, "draws": 0, "losses": 1})
        self.assertEqual(evidence.termination_counts["infrastructure_unknown"]["unterminated"], 1)

    def test_missing_pgn_game_is_audited_and_quarantined(self) -> None:
        pgn = textwrap.dedent(
            """
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

            evidence = parse_paired_pgn(path, "Blaze", "Opponent", expected_games=2)

        self.assertEqual(evidence.completed_games, 1)
        self.assertEqual(evidence.quarantined_pairs, 1)
        self.assertEqual(evidence.raw_wdl, {"wins": 1, "draws": 0, "losses": 0})
        self.assertEqual(evidence.abnormal_games[0]["game_index"], 0)
        self.assertEqual(evidence.abnormal_games[0]["reason"], "PGN game is missing")

    def test_malformed_movetext_is_infrastructure_unknown(self) -> None:
        pgn = textwrap.dedent(
            """
            [Event "paired"]
            [Round "1"]
            [White "Blaze"]
            [Black "Opponent"]
            [Result "1-0"]

            1. e5 1-0

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

            with self.assertLogs("chess.pgn", level="ERROR"):
                evidence = parse_paired_pgn(path, "Blaze", "Opponent", expected_games=2)

        self.assertEqual(evidence.clean_pairs, 0)
        self.assertEqual(evidence.termination_counts["infrastructure_unknown"]["malformed"], 1)

    def test_rejects_header_and_movetext_result_disagreement(self) -> None:
        pgn = textwrap.dedent(
            """
            [Event "paired"]
            [Round "1"]
            [White "Blaze"]
            [Black "Opponent"]
            [Result "1-0"]

            1. e4 e5 0-1

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

            with self.assertRaisesRegex(ValueError, "movetext result"):
                parse_paired_pgn(path, "Blaze", "Opponent", expected_games=2)

    def test_rejects_duplicate_round_slots_even_when_game_count_is_complete(self) -> None:
        pgn = textwrap.dedent(
            """
            [Event "paired"]
            [Round "1"]
            [White "Blaze"]
            [Black "Opponent"]
            [Result "1/2-1/2"]

            1. e4 e5 1/2-1/2

            [Event "paired"]
            [Round "1"]
            [White "Opponent"]
            [Black "Blaze"]
            [Result "1/2-1/2"]

            1. e4 e5 1/2-1/2
            """
        ).strip()
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "games.pgn"
            path.write_text(pgn + "\n", encoding="utf-8")

            with self.assertRaisesRegex(ValueError, "duplicate PGN Round"):
                parse_paired_pgn(path, "Blaze", "Opponent", expected_games=2)

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

        self.assertEqual(result.counts.as_tuple(), (1, 0, 0, 0, 0))

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
    def test_runner_failure_returns_zero_evidence_without_calling_sprt(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            candidate = root / "candidate.exe"
            opponent = root / "opponent.exe"
            runner = root / "runner.exe"
            openings = root / "openings.epd"
            for path in (candidate, opponent, runner):
                path.write_bytes(path.name.encode("ascii"))
            openings.write_text(
                'rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - id "start";\n',
                encoding="utf-8",
            )
            spec = MatchSpec(
                schema_version=1,
                name="failed-runner",
                games=4,
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

            def failed_executor(command: list[str], **_: object) -> subprocess.CompletedProcess[str]:
                pgn_path = Path(command[command.index("-pgnout") + 1])
                pgn_path.write_text(
                    '[Event "partial"]\n[Round "1"]\n[White "Blaze"]\n'
                    '[Black "Opponent"]\n[Result "0-1"]\n[Termination "time forfeit"]\n\n'
                    '1. e4 e5 0-1\n\n'
                    '[Event "partial"]\n[Round "2"]\n[White "Opponent"]\n'
                    '[Black "Blaze"]\n[Result "0-1"]\n\n1. e4 e5 0-1\n',
                    encoding="utf-8",
                )
                return subprocess.CompletedProcess(command, 17, stdout="runner crashed\n")

            with patch("tools.experiment.match.sprt_llr", side_effect=AssertionError("SPRT called")), patch(
                "tools.experiment.match.sprt_decision", side_effect=AssertionError("SPRT called")
            ):
                result = run_match(
                    spec,
                    candidate=candidate,
                    opponent=opponent,
                    runner=runner,
                    output=root / "result",
                    source_commit="0" * 40,
                    executor=failed_executor,
                )

        self.assertEqual(result.decision, "no_clean_pairs")
        self.assertEqual(result.llr, 0.0)
        self.assertEqual(result.evidence.completed_games, 2)
        self.assertEqual(result.evidence.raw_wdl, {"wins": 1, "draws": 0, "losses": 1})
        self.assertEqual(result.evidence.quarantined_pairs, 2)
        self.assertEqual(result.evidence.abnormal_games[0]["category"], "time_loss")
        self.assertEqual(result.evidence.abnormal_games[0]["offender"], "candidate")
        self.assertEqual(result.evidence.abnormal_games[1]["category"], "runner_failure")
        self.assertEqual(result.evidence.abnormal_games[1]["result"], "0-1")
        self.assertEqual(result.evidence.abnormal_games[1]["candidate_color"], "black")
        self.assertEqual(result.evidence.abnormal_games[2]["result"], "*")
        self.assertEqual(
            result.evidence.termination_counts["infrastructure_unknown"]["runner_failure"],
            3,
        )

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
