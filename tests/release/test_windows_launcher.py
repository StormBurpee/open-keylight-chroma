"""Run the real Windows PowerShell launcher regression harness offline."""
import json
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
import zipfile

ROOT = Path(__file__).resolve().parents[2]


@unittest.skipUnless(os.name == "nt", "Windows launcher uses Windows PowerShell and native executables")
class LauncherTests(unittest.TestCase):
    def test_actual_cmd_from_powershell7_keeps_inherited_modules_and_uses_cache(self):
        pwsh = shutil.which("pwsh.exe")
        node = shutil.which("node.exe")
        self.assertIsNotNone(pwsh, "PowerShell 7 is required for the inherited module-path regression")
        self.assertIsNotNone(node, "A local Node executable is required; nothing is downloaded")
        with tempfile.TemporaryDirectory(prefix="okl ps7 cmd launch ") as temporary:
            directory = Path(temporary)
            release = directory / "release with spaces"
            cache = directory / "local app data/OpenKeylight/runtime-archives"
            cache.mkdir(parents=True)
            for name in ("installer", "tools", "firmware"):
                (release / name).mkdir(parents=True)
            for name in ("start-open-keylight.cmd", "start-open-keylight.ps1"):
                shutil.copyfile(ROOT / "distribution/windows" / name, release / name)
            (release / "tools/stock_migration.py").write_text("# Never executed by this demo fixture.\n")
            (release / "firmware/bundle.json").write_text("{}")
            (release / "installer/cli.js").write_text(
                'const fs=require("node:fs");const args=process.argv.slice(2);'
                'if(!args.includes("--demo")||!args.includes("discover"))process.exit(3);'
                'fs.writeFileSync(process.env.OKL_TEST_CAPTURE,JSON.stringify(args));'
                'console.log("OFFLINE_DEMO_OK");')
            manifest = {"format": 1, "platform": "win-x64"}
            for kind, filename, content in (("node", "node.exe", Path(node).read_bytes()),
                                             ("python", "python.exe", b"not executed")):
                archive = directory / f"{kind}.zip"
                with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED) as stream:
                    stream.writestr(filename, content)
                raw = archive.read_bytes()
                digest = hashlib.sha256(raw).hexdigest()
                shutil.copyfile(archive, cache / f"{digest}.zip")
                manifest[kind] = {"url": "https://www.python.org/fixture.zip", "bytes": len(raw),
                                  "sha256": digest, "prefix": ""}
            (release / "runtimes.json").write_text(json.dumps(manifest))
            driver = directory / "invoke.ps1"
            driver.write_text(
                'param([string]$Launcher,[string]$Evidence)\n'
                '$before=$env:PSModulePath\n'
                '& $Launcher --demo discover --columns 80\n'
                '$result=$LASTEXITCODE\n'
                '@{edition=$PSEdition;before=$before;after=$env:PSModulePath;result=$result} | '
                'ConvertTo-Json | Set-Content -LiteralPath $Evidence\n'
                'exit $result\n')
            evidence = directory / "environment.json"
            capture = directory / "argv.json"
            environment = os.environ.copy()  # Deliberately preserve PS7's inherited PSModulePath.
            environment.update(LOCALAPPDATA=str(directory / "local app data"), OKL_TEST_CAPTURE=str(capture))
            result = subprocess.run([pwsh, "-NoProfile", "-File", str(driver),
                                     "-Launcher", str(release / "start-open-keylight.cmd"),
                                     "-Evidence", str(evidence)], input="\n", capture_output=True,
                                    text=True, timeout=45, env=environment)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("OFFLINE_DEMO_OK", result.stdout)
            details = json.loads(evidence.read_text(encoding="utf-8-sig"))
            self.assertEqual(details["edition"], "Core")
            self.assertEqual(details["before"], details["after"])
            self.assertEqual(details["result"], 0)
            self.assertEqual(json.loads(capture.read_text())[-4:], ["--demo", "discover", "--columns", "80"])
            self.assertEqual(list(cache.glob("session-*")), [])
            self.assertEqual(list(cache.glob("*.download")), [])

    def test_actual_powershell_launcher(self):
        powershell = shutil.which("powershell.exe")
        node = shutil.which("node.exe")
        self.assertIsNotNone(powershell, "Windows PowerShell is required")
        self.assertIsNotNone(node, "Local Node is required for the isolated launch fixture; no download occurs")
        # Cover Explorer's Windows PowerShell defaults here; the separate real
        # PS7 -> CMD -> PS5.1 test deliberately preserves inherited module paths.
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
