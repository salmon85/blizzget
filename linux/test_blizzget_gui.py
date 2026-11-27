import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from blizzget_gui import build_command, validate_inputs


class BlizzGetGuiTests(unittest.TestCase):
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
