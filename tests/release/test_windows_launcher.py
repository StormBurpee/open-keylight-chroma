"""Run the real Windows PowerShell launcher regression harness offline."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


@unittest.skipUnless(os.name == "nt", "Windows launcher uses Windows PowerShell and native executables")
class LauncherTests(unittest.TestCase):
    def test_actual_powershell_launcher(self):
        powershell = shutil.which("powershell.exe")
        node = shutil.which("node.exe")
        self.assertIsNotNone(powershell, "Windows PowerShell is required")
        self.assertIsNotNone(node, "Local Node is required for the isolated launch fixture; no download occurs")
        # PowerShell 7 injects Core-only module paths into its children. A
        # double-clicked CMD launcher instead gets Windows PowerShell defaults.
        environment = {key: value for key, value in os.environ.items() if key.lower() != "psmodulepath"}
        result = subprocess.run([powershell, "-NoLogo", "-NoProfile", "-ExecutionPolicy", "Bypass",
                                 "-File", str(ROOT / "tests/release/test_windows_launcher.ps1"), "-NodeExe", node],
                                capture_output=True, text=True, timeout=60, cwd=ROOT, env=environment)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        lines = [line.removeprefix("LAUNCHER_TEST_RESULT ") for line in result.stdout.splitlines()
                 if line.startswith("LAUNCHER_TEST_RESULT ")]
        self.assertEqual(len(lines), 1, result.stdout)
        evidence = json.loads(lines[0])
        self.assertEqual(evidence["result"], "pass")
        self.assertEqual(evidence["network_requests"], 0)
        self.assertGreaterEqual(evidence["checks"], 95)
        self.assertGreaterEqual(evidence["cases"], 18)

    def test_cmd_preserves_failure_when_release_is_incomplete(self):
        with tempfile.TemporaryDirectory(prefix="okl cmd launcher ") as temporary:
            directory = Path(temporary)
            for name in ("start-open-keylight.cmd", "start-open-keylight.ps1"):
                shutil.copyfile(ROOT / "distribution/windows" / name, directory / name)
            # This isolated directory has no installer/bundle: the real script
            # must fail before its runtime download path and preserve exit 1.
            environment = {key: value for key, value in os.environ.items() if key.lower() != "psmodulepath"}
            result = subprocess.run(["cmd.exe", "/d", "/c", str(directory / "start-open-keylight.cmd")],
                                    input="\n", capture_output=True, text=True, timeout=15, env=environment)
            self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
            self.assertIn("Extract the entire release ZIP", result.stdout)
            self.assertNotIn("Preparing your local installer", result.stdout)


if __name__ == "__main__":
    unittest.main()
