#!/usr/bin/env python3
"""Build a deterministic Hailstone addon package. Requires Python 3.10+ only."""
import argparse
import hashlib
import pathlib
import re
import subprocess
import zipfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
VERSION = re.compile(r"\d+\.\d+\.\d+(?:-[0-9A-Za-z.-]+)?(?:\+[0-9A-Za-z.-]+)?")


def package(build, version, platform, out):
    if not VERSION.fullmatch(version):
        raise ValueError("Use a semantic version, for example 1.0.0-preview.1")
    extension = {"windows-x64": ".dll", "linux-x64": ".so", "macos-arm64": ".dylib"}.get(platform)
    if not extension:
        raise ValueError("Unsupported platform")
    library = build / ("sentinel" + extension)
    if not library.is_file():
        raise ValueError(f"Build the addon first: missing {library}")
    entries = [(library.name, library)]
    entries += [(p.relative_to(ROOT).as_posix(), p) for p in sorted((ROOT / "ui").rglob("*.lua"))]
    entries += [("LICENSE.txt", ROOT / "LICENSE"), ("NOTICE.txt", ROOT / "THIRD_PARTY.md")]
    out.mkdir(parents=True, exist_ok=True)
    archive = out / f"sentinel-{version}-{platform}.zip"
    with zipfile.ZipFile(archive, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for name, path in sorted(entries):
            item = zipfile.ZipInfo(name, date_time=(1980, 1, 1, 0, 0, 0))
            item.compress_type = zipfile.ZIP_DEFLATED
            item.external_attr = 0o100644 << 16
            z.writestr(item, path.read_bytes())
    data = archive.read_bytes()
    manifest = out / "hailstone-addon.txt"
    manifest.write_text(
        "hailstone-addon 1\n"
        "name sentinel\n"
        "kind plugin\n"
        f"version {version}\n"
        "author Fryguy\n"
        "description Personal alerts for health, buffs, combat and chat.\n"
        "min_client 0.7.0-beta\n"
        "caps read,commands,events,ui\n"
        f"package {platform} {len(data)} {hashlib.sha256(data).hexdigest()} {archive.name}\n",
        encoding="utf-8", newline="\n")
    return manifest, archive


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--build-dir", type=pathlib.Path, default=ROOT / "build")
    p.add_argument("--version", default="1.0.0-preview.2")
    p.add_argument("--platform", default="windows-x64")
    p.add_argument("--out", type=pathlib.Path, default=ROOT / "release")
    p.add_argument("--sign-tool", type=pathlib.Path)
    p.add_argument("--sign-key", type=pathlib.Path, help="Existing private addon key, kept outside the repository")
    p.add_argument("--root-manifest", action="store_true", help="Also write the repository manifest pointing at this release's asset")
    a = p.parse_args()
    if bool(a.sign_tool) != bool(a.sign_key):
        p.error("Provide both --sign-tool and --sign-key")
    try:
        manifest, archive = package(a.build_dir, a.version, a.platform, a.out)
        manifests = [manifest]
        if a.root_manifest:
            root_manifest = ROOT / "hailstone-addon.txt"
            asset = f"https://github.com/fryguy503/hailstone-sentinel/releases/download/v{a.version}/{archive.name}"
            root_manifest.write_text(manifest.read_text(encoding="utf-8").replace(archive.name, asset), encoding="utf-8", newline="\n")
            manifests.append(root_manifest)
        if a.sign_tool:
            for item in manifests:
                subprocess.run([str(a.sign_tool), "sign", str(a.sign_key), str(item)], check=True)
        print(manifest)
        print(archive)
        if not a.sign_tool:
            print("Unsigned package; Hailstone shows this during installation.")
    except (OSError, ValueError, subprocess.CalledProcessError) as e:
        p.exit(1, f"Package failed: {e}\n")


if __name__ == "__main__":
    main()
