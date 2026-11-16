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
    def platform_packages(self, base, version="1.0.0-preview.4"):
        manifests = []
        for platform, extension in pack.PLATFORMS.items():
            build = base / platform / "build"
            build.mkdir(parents=True)
            (build / ("sentinel" + extension)).write_bytes((platform + "-native-fixture").encode())
            manifest, _ = pack.package(build, version, platform, base / platform / "package")
            manifests.append(manifest)
        return manifests

    def rewrite_archive(self, manifest, change):
        record = next(line for line in manifest.read_text().splitlines() if line.startswith("package ")).split()
        archive = manifest.parent / record[4]
        with zipfile.ZipFile(archive) as z:
            files = {name: z.read(name) for name in z.namelist()}
        change(files)
        with zipfile.ZipFile(archive, "w", compression=zipfile.ZIP_DEFLATED) as z:
            for name, data in files.items():
                z.writestr(name, data)
        data = archive.read_bytes()
        old = " ".join(record)
        record[2], record[3] = str(len(data)), hashlib.sha256(data).hexdigest()
        manifest.write_text(manifest.read_text().replace(old, " ".join(record)), encoding="utf-8")

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

    def test_three_platform_merge_is_deterministic_and_preserves_assets(self):
        with tempfile.TemporaryDirectory() as d:
            base = Path(d)
            inputs = self.platform_packages(base)
            manifest, archives = pack.merge(inputs, base / "combined")
            original = manifest.read_bytes()
            records = [line.split() for line in manifest.read_text().splitlines() if line.startswith("package ")]
            self.assertEqual([record[1] for record in records], sorted(pack.PLATFORMS))
            self.assertEqual(len(archives), 3)
            for record in records:
                archive = base / "combined" / record[4]
                source = next(path.parent / record[4] for path in inputs if (path.parent / record[4]).exists())
                self.assertEqual(archive.read_bytes(), source.read_bytes())
                self.assertEqual(int(record[2]), archive.stat().st_size)
                self.assertEqual(record[3], hashlib.sha256(archive.read_bytes()).hexdigest())
            other, _ = pack.merge(list(reversed(inputs)), base / "reordered")
            self.assertEqual(original, other.read_bytes())
            root = pack.write_root_manifest(manifest, base / "root-manifest.txt")
            root_records = [line.split() for line in root.read_text().splitlines() if line.startswith("package ")]
            for record in root_records:
                self.assertTrue(record[4].startswith(pack.REPOSITORY + "/releases/download/v1.0.0-preview.4/"))
                self.assertEqual(record[2:4], next(item[2:4] for item in records if item[1] == record[1]))

    def test_merge_rejects_missing_and_duplicate_platforms_without_output(self):
        with tempfile.TemporaryDirectory() as d:
            base = Path(d)
            inputs = self.platform_packages(base)
            for manifests in (inputs[:2], inputs + inputs[:1], []):
                with self.subTest(manifests=manifests), self.assertRaises(ValueError):
                    pack.merge(manifests, base / "output")
                self.assertFalse((base / "output").exists())
            manifest, _ = pack.merge(inputs[:1], base / "explicit-subset", ["windows-x64"])
            self.assertEqual(manifest.read_text().count("\npackage "), 1)

    def test_merge_rejects_mismatched_release_metadata(self):
        with tempfile.TemporaryDirectory() as d:
            base = Path(d)
            inputs = self.platform_packages(base)
            original = inputs[-1].read_text()
            for before, after in (("version 1.0.0-preview.4", "version 1.0.0-preview.5"),
                                  ("min_client 0.7.0-beta", "min_client 0.8.0-beta"),
                                  ("caps read,commands,events,ui", "caps read,events,ui"),
                                  ("name sentinel", "name different"),
                                  ("kind plugin", "kind script"),
                                  ("author Fryguy", "author Someone Else")):
                inputs[-1].write_text(original.replace(before, after), encoding="utf-8")
                with self.subTest(after=after), self.assertRaises(ValueError):
                    pack.merge(inputs, base / "output")
                self.assertFalse((base / "output").exists())
            inputs[-1].write_text(original.replace("caps read,commands,events,ui", "caps ui,events,read,commands"), encoding="utf-8")
            self.assertTrue(pack.merge(inputs, base / "same-capabilities")[0].is_file())

    def test_merge_rejects_missing_tampered_or_wrong_sized_assets(self):
        with tempfile.TemporaryDirectory() as d:
            base = Path(d)
            inputs = self.platform_packages(base)
            manifest = inputs[-1]
            record = next(line for line in manifest.read_text().splitlines() if line.startswith("package ")).split()
            archive = manifest.parent / record[4]
            original = archive.read_bytes()
            for data in (original + b"extra", bytes([original[0] ^ 1]) + original[1:], None):
                if data is None:
                    archive.unlink()
                else:
                    archive.write_bytes(data)
                with self.subTest(data="missing" if data is None else len(data)), self.assertRaises(ValueError):
                    pack.merge(inputs, base / "output")
                self.assertFalse((base / "output").exists())

    def test_merge_rejects_shared_source_mismatch_even_with_valid_hash(self):
        with tempfile.TemporaryDirectory() as d:
            base = Path(d)
            inputs = self.platform_packages(base)
            self.rewrite_archive(inputs[-1], lambda files: files.update({"ui/modern/sentinel.lua": b"return 'different source'"}))
            with self.assertRaisesRegex(ValueError, "different shared"):
                pack.merge(inputs, base / "output")
            self.assertFalse((base / "output").exists())

    def test_merge_rejects_unsafe_or_incomplete_zip_with_valid_hash(self):
        for name in ("../outside.lua", "ui/../outside.lua", "ui//extra.lua", "other.dll", "ui/payload.exe"):
            with self.subTest(name=name), tempfile.TemporaryDirectory() as d:
                base = Path(d)
                inputs = self.platform_packages(base)
                self.rewrite_archive(inputs[-1], lambda files: files.update({name: b"invalid"}))
                with self.assertRaises(ValueError):
                    pack.merge(inputs, base / "output")
                self.assertFalse((base / "output").exists())
        with tempfile.TemporaryDirectory() as d:
            base = Path(d)
            inputs = self.platform_packages(base)
            self.rewrite_archive(inputs[-1], lambda files: files.pop("sentinel.dylib"))
            with self.assertRaisesRegex(ValueError, "missing required"):
                pack.merge(inputs, base / "output")

    def test_merge_rejects_signed_or_malformed_manifest_and_preserves_existing_output(self):
        with tempfile.TemporaryDirectory() as d:
            base = Path(d)
            inputs = self.platform_packages(base)
            out = base / "output"
            out.mkdir()
            previous = out / "hailstone-addon.txt"
            previous.write_text("keep existing release", encoding="utf-8")
            original = inputs[-1].read_text()
            mutations = [original + "key " + "0" * 64 + "\n", original + "version 1.0.0\n",
                         original.replace("hailstone-addon 1", "hailstone-addon 99"),
                         original.replace("macos-arm64", "macos-x64"),
                         original.replace("sentinel-1.0.0-preview.4-macos-arm64.zip", "../sentinel.zip")]
            for content in mutations:
                inputs[-1].write_text(content, encoding="utf-8")
                with self.subTest(content=content[-90:]), self.assertRaises(ValueError):
                    pack.merge(inputs, out)
                self.assertEqual(previous.read_text(), "keep existing release")
                self.assertEqual(list(out.iterdir()), [previous])


if __name__ == "__main__":
    unittest.main()
