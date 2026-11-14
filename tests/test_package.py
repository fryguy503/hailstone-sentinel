import hashlib
import importlib.util
from pathlib import Path
import tempfile
import unittest
import zipfile

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("sentinel_package", ROOT / "tools" / "package.py")
pack = importlib.util.module_from_spec(spec)
spec.loader.exec_module(pack)


class Packaging(unittest.TestCase):
    def test_manifest_and_deterministic_archive(self):
        with tempfile.TemporaryDirectory() as d:
            base = Path(d)
            build = base / "build"
            build.mkdir()
            (build / "sentinel.dll").write_bytes(b"MZ-test-fixture")
            manifest, archive = pack.package(build, "1.0.0-preview.1", "windows-x64", base / "out")
            original = archive.read_bytes()
            lines = manifest.read_text().splitlines()
            self.assertEqual(lines[0], "hailstone-addon 1")
            self.assertIn("caps read,commands,events,ui", lines)
            record = next(line for line in lines if line.startswith("package ")).split()
            self.assertEqual(int(record[2]), len(original))
            self.assertEqual(record[3], hashlib.sha256(original).hexdigest())
            with zipfile.ZipFile(archive) as z:
                names = z.namelist()
                self.assertEqual([n for n in names if n.endswith(".dll")], ["sentinel.dll"])
                self.assertIn("ui/modern/sentinel.lua", names)
                self.assertIn("ui/gamepad/sentinel.lua", names)
                self.assertFalse(any(".." in n or n.startswith("/") for n in names))
            pack.package(build, "1.0.0-preview.1", "windows-x64", base / "out")
            self.assertEqual(original, archive.read_bytes())

    def test_missing_build_and_bad_version(self):
        with tempfile.TemporaryDirectory() as d:
            for version, platform in [("1.0.0", "windows-x64"), ("../bad", "windows-x64"), ("1.0.0", "unknown")]:
                with self.assertRaises(ValueError):
                    pack.package(Path(d), version, platform, Path(d) / "out")


if __name__ == "__main__":
    unittest.main()
