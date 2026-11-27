import sys
import unittest
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parent))

from blizzget_gui import (
    VERSION_PRESETS,
    build_command,
    merge_version_records,
    parse_seqn_history,
    parse_version_entries,
    validate_concurrency,
    validate_inputs,
)


class BlizzGetGuiTests(unittest.TestCase):
    def test_parse_seqn_history(self) -> None:
        page = {
            "results": [
                {"seqn": 2938946},
                {"seqn": 2934402},
                {"seqn": 2938946},
            ]
        }

        self.assertEqual(parse_seqn_history(page), [2938946, 2934402])

    def test_parse_version_entries_for_region(self) -> None:
        entries = [
            {
                "region": "us",
                "version_name": "2.8.0.99920",
                "build_config": "e34fc3fb7831a77e0ad7def0bb399ac7",
                "cdn_config": "6547fc64ca796c7c1b25864145814ace",
            },
            {
                "region": "eu",
                "version_name": "2.8.0.99920",
                "build_config": "e34fc3fb7831a77e0ad7def0bb399ac7",
                "cdn_config": "6547fc64ca796c7c1b25864145814ace",
            },
            {
                "region": "us",
                "version_name": "2.7.7.12345",
                "build_config": "11111111111111111111111111111111",
                "cdn_config": "22222222222222222222222222222222",
            },
        ]

        records = parse_version_entries(entries, "us")

        self.assertEqual(len(records), 2)
        self.assertEqual(records[0]["build_config"], "e34fc3fb7831a77e0ad7def0bb399ac7")
        self.assertEqual(records[1]["version"], "2.7.7.12345")

    def test_merge_version_records_deduplicates(self) -> None:
        first = {"build_config": "a" * 32, "version": "2.8.0.1"}
        duplicate = {"build_config": "a" * 32, "version": "2.8.0.1"}
        older = {"build_config": "b" * 32, "version": "2.7.4.1"}

        merged = merge_version_records([[first, older], [duplicate]])

        self.assertEqual(len(merged), 2)
        self.assertEqual(merged[0], first)

    def test_version_presets(self) -> None:
        self.assertEqual(
            VERSION_PRESETS["Diablo III 2.7.4.84161"],
            ("5194054cfa6480884c9a451b6f1107a3", "6547fc64ca796c7c1b25864145814ace"),
        )
        self.assertEqual(
            VERSION_PRESETS["Diablo III 2.8.0.99920"],
            ("e34fc3fb7831a77e0ad7def0bb399ac7", "6547fc64ca796c7c1b25864145814ace"),
        )
        self.assertIsNone(VERSION_PRESETS["Custom"])

    def test_validate_concurrency(self) -> None:
        self.assertIsNone(validate_concurrency("96", "24"))
        self.assertIn("positive", validate_concurrency("0", "12") or "")
        self.assertIn("per-host", validate_concurrency("12", "24") or "")
        self.assertIn("number", validate_concurrency("many", "12") or "")

    def test_frozen_runner_uses_cli_dispatch(self) -> None:
        with patch.object(sys, "frozen", True, create=True):
            command = build_command(
                build_config="5194054cfa6480884c9a451b6f1107a3",
                cdn_config="6547fc64ca796c7c1b25864145814ace",
                output="/tmp/d3",
                product="d3",
                region="us",
                platform="Windows",
                arch="x86_64",
                locale="enUS",
            )

        self.assertEqual(command[:2], [sys.executable, "--cli"])

    def test_build_command(self) -> None:
        command = build_command(
            build_config="5194054cfa6480884c9a451b6f1107a3",
            cdn_config="6547fc64ca796c7c1b25864145814ace",
            output="/home/test/Diablo III 2.7.4",
            product="d3",
            region="us",
            platform="Windows",
            arch="x86_64",
            locale="enUS",
        )

        self.assertEqual(
            command[:5],
            [sys.executable, "-m", "cascette_tools", "install", "install-product"],
        )
        self.assertIn("/home/test/Diablo III 2.7.4", command)
        self.assertEqual(command[command.index("--product") + 1], "d3")
        self.assertEqual(command[command.index("--subfolder") + 1], "")

    def test_validate_inputs(self) -> None:
        self.assertIsNone(
            validate_inputs(
                "5194054cfa6480884c9a451b6f1107a3",
                "6547fc64ca796c7c1b25864145814ace",
                "/home/test/Diablo III",
            )
        )
        build_error = validate_inputs(
            "bad", "6547fc64ca796c7c1b25864145814ace", "/tmp/d3"
        )
        self.assertIsNotNone(build_error)
        self.assertIn("Build config", build_error or "")
        output_error = validate_inputs(
            "5194054cfa6480884c9a451b6f1107a3", "6547fc64ca796c7c1b25864145814ace", ""
        )
        self.assertIsNotNone(output_error)
        self.assertIn("output", output_error or "")


if __name__ == "__main__":
    unittest.main()
