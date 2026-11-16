#!/usr/bin/env python3
"""Build or merge deterministic Hailstone addon packages. Requires Python 3.10+."""
import argparse
import hashlib
import io
import pathlib
import re
import subprocess
import zipfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
VERSION = re.compile(r"\d+\.\d+\.\d+(?:-[0-9A-Za-z.-]+)?(?:\+[0-9A-Za-z.-]+)?")
DEFAULT_VERSION = "1.0.0-preview.4"
PLATFORMS = {"windows-x64": ".dll", "linux-x64": ".so", "macos-arm64": ".dylib"}
FIELDS = ("name", "kind", "version", "author", "description", "min_client", "caps")
CAPABILITIES = ("read", "commands", "target", "actions", "move", "chat", "events", "ui",
                "packets", "extend", "login", "net", "web")
REPOSITORY = "https://github.com/fryguy503/hailstone-sentinel"
MAX_ARCHIVE_BYTES = 64 * 1024 * 1024


def _check_version(version):
    if not isinstance(version, str) or len(version) > 96 or not VERSION.fullmatch(version):
        raise ValueError("Use a semantic version, for example 1.0.0-preview.4")


def _manifest_text(metadata, records, absolute=False):
    lines = ["hailstone-addon 1"]
    lines += [f"{key} {metadata[key]}" for key in FIELDS]
    if "notes" in metadata:
        lines.append(f"notes {metadata['notes']}")
    for platform, size, digest, filename in sorted(records):
        asset = f"{REPOSITORY}/releases/download/v{metadata['version']}/{filename}" if absolute else filename
        lines.append(f"package {platform} {size} {digest} {asset}")
    return "\n".join(lines) + "\n"


def package(build, version, platform, out):
    """Create one platform package; retained for existing build callers."""
    _check_version(version)
    extension = PLATFORMS.get(platform)
    if not extension:
        raise ValueError("Unsupported platform")
    build, out = pathlib.Path(build), pathlib.Path(out)
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
    metadata = {"name": "sentinel", "kind": "plugin", "version": version, "author": "Fryguy",
                "description": "Personal alerts for health, buffs, combat and chat.",
                "min_client": "0.7.0-beta", "caps": "read,commands,events,ui"}
    record = (platform, len(data), hashlib.sha256(data).hexdigest(), archive.name)
    manifest = out / "hailstone-addon.txt"
    manifest.write_text(_manifest_text(metadata, [record]), encoding="utf-8", newline="\n")
    return manifest, archive


def _read_manifest(path):
    path = pathlib.Path(path)
    if path.stat().st_size > 128 * 1024:
        raise ValueError(f"Manifest is too large: {path}")
    lines = path.read_text(encoding="utf-8").splitlines()
    if not lines or lines[0] != "hailstone-addon 1":
        raise ValueError(f"Not a Hailstone addon manifest: {path}")
    metadata, records = {}, []
    for line in lines[1:]:
        if not line.strip():
            continue
        key, separator, value = line.partition(" ")
        if not separator or not value.strip():
            raise ValueError(f"Malformed manifest line: {line}")
        value = value.strip()
        if key in ("key", "signature"):
            raise ValueError("Merge unsigned platform manifests, then sign the combined manifest")
        if key == "package":
            fields = value.split()
            if len(fields) != 4:
                raise ValueError("Malformed package record")
            platform, size, digest, filename = fields
            if platform not in PLATFORMS:
                raise ValueError(f"Unsupported platform: {platform}")
            if not size.isdecimal() or not 0 < int(size) <= MAX_ARCHIVE_BYTES:
                raise ValueError("Invalid package size")
            if not re.fullmatch(r"[0-9a-f]{64}", digest):
                raise ValueError("Invalid package SHA-256")
            records.append((platform, int(size), digest, filename))
        elif key in FIELDS or key == "notes":
            if key in metadata:
                raise ValueError(f"Duplicate metadata field: {key}")
            metadata[key] = value
        else:
            raise ValueError(f"Unknown manifest field: {key}")
    if any(key not in metadata for key in FIELDS) or not records:
        raise ValueError("Manifest metadata or packages are missing")
    if metadata["name"] != "sentinel" or metadata["kind"] != "plugin":
        raise ValueError("Expected the Sentinel plugin manifest")
    _check_version(metadata["version"])
    _check_version(metadata["min_client"])
    for platform, _, _, filename in records:
        if filename != f"sentinel-{metadata['version']}-{platform}.zip":
            raise ValueError("Use the adjacent ZIP filename from the platform manifest")
    caps = metadata["caps"].split(",")
    if not caps or len(caps) != len(set(caps)) or any(cap not in CAPABILITIES for cap in caps):
        raise ValueError("Unknown or duplicate capabilities")
    metadata["caps"] = ",".join(cap for cap in CAPABILITIES if cap in caps)
    return metadata, records


def _archive_payload(data, platform):
    """Validate installation shape without extracting files or executing native code."""
    library = "sentinel" + PLATFORMS[platform]
    try:
        with zipfile.ZipFile(io.BytesIO(data)) as archive:
            entries = archive.infolist()
            names = [entry.filename for entry in entries]
            if len(names) != len(set(names)) or len(names) > 512:
                raise ValueError("Archive contains duplicate or excessive entries")
            required = {library, "LICENSE.txt", "NOTICE.txt", "ui/modern/sentinel.lua", "ui/gamepad/sentinel.lua"}
            if not required.issubset(names):
                raise ValueError(f"Archive is missing required files for {platform}")
            if sum(entry.file_size for entry in entries) > MAX_ARCHIVE_BYTES:
                raise ValueError("Unpacked archive exceeds 64 MiB")
            for entry in entries:
                name, parts = entry.filename, entry.filename.split("/")
                if (name.startswith("/") or "\\" in name or ":" in name or
                        any(part in ("", ".", "..") for part in parts)):
                    raise ValueError(f"Unsafe archive filename: {name}")
                mode = (entry.external_attr >> 16) & 0o170000
                if (mode not in (0, 0o100000) or entry.flag_bits & 1 or
                        entry.compress_type not in (zipfile.ZIP_STORED, zipfile.ZIP_DEFLATED)):
                    raise ValueError(f"Unsupported archive entry: {name}")
                if name not in required and not (name.startswith("ui/") and name.endswith(".lua")):
                    raise ValueError(f"Unexpected archive file: {name}")
            archive.read(library)  # Check the native library's ZIP CRC as well as the shared resources.
            return {name: archive.read(name) for name in names if name != library}
    except (zipfile.BadZipFile, RuntimeError, NotImplementedError) as error:
        raise ValueError(f"Invalid ZIP archive: {error}") from error


def merge(manifests, out, required_platforms=None):
    """Verify and combine artifacts; no output changes until every input validates."""
    required = set(PLATFORMS if required_platforms is None else required_platforms)
    if not required or not required.issubset(PLATFORMS):
        raise ValueError("Required platforms must be supported Sentinel platforms")
    metadata, records, assets, common = None, [], {}, None
    for manifest in manifests:
        path = pathlib.Path(manifest)
        current, packages = _read_manifest(path)
        if metadata is None:
            metadata = current
        elif metadata != current:
            raise ValueError("Platform manifests have different release metadata")
        for record in packages:
            platform, size, digest, filename = record
            if platform in assets:
                raise ValueError(f"Duplicate platform: {platform}")
            expected = f"sentinel-{current['version']}-{platform}.zip"
            if filename != expected:
                raise ValueError("Use the adjacent ZIP filename from the platform manifest")
            asset = path.parent / filename
            if not asset.is_file() or asset.is_symlink():
                raise ValueError(f"Package is missing or not a regular file: {asset}")
            if asset.stat().st_size != size:
                raise ValueError(f"Package size mismatch: {asset}")
            data = asset.read_bytes()
            if hashlib.sha256(data).hexdigest() != digest:
                raise ValueError(f"Package SHA-256 mismatch: {asset}")
            shared = _archive_payload(data, platform)
            if common is None:
                common = shared
            elif shared != common:
                raise ValueError("Platform archives contain different shared UI or license files")
            assets[platform] = (filename, data)
            records.append(record)
    if set(assets) != required:
        missing, extra = sorted(required - set(assets)), sorted(set(assets) - required)
        raise ValueError(f"Platform set mismatch; missing={missing}, unexpected={extra}")
    out = pathlib.Path(out)
    out.mkdir(parents=True, exist_ok=True)
    archives = []
    for platform in sorted(assets):
        filename, data = assets[platform]
        archive = out / filename
        archive.write_bytes(data)
        archives.append(archive)
    result = out / "hailstone-addon.txt"
    result.write_text(_manifest_text(metadata, records), encoding="utf-8", newline="\n")
    return result, archives


def write_root_manifest(manifest, destination=None):
    """Write repository discovery metadata with explicit release asset URLs."""
    metadata, records = _read_manifest(manifest)
    destination = pathlib.Path(destination) if destination is not None else ROOT / "hailstone-addon.txt"
    destination.write_text(_manifest_text(metadata, records, absolute=True), encoding="utf-8", newline="\n")
    return destination


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--build-dir", type=pathlib.Path, default=ROOT / "build")
    p.add_argument("--version", help=f"Package version (default {DEFAULT_VERSION}); merges infer it from the manifests")
    p.add_argument("--platform", choices=PLATFORMS, default="windows-x64")
    p.add_argument("--out", type=pathlib.Path, default=ROOT / "release")
    p.add_argument("--merge", type=pathlib.Path, nargs="+", metavar="MANIFEST", help="Verify and merge downloaded platform manifests and adjacent ZIPs")
    p.add_argument("--require-platforms", choices=PLATFORMS, nargs="+", help="Exact merge platform set (default all three)")
    p.add_argument("--sign-tool", type=pathlib.Path)
    p.add_argument("--sign-key", type=pathlib.Path, help="Existing private addon key, kept outside the repository")
    p.add_argument("--root-manifest", action="store_true", help="Also write repository metadata with explicit release asset URLs")
    a = p.parse_args()
    if bool(a.sign_tool) != bool(a.sign_key):
        p.error("Provide both --sign-tool and --sign-key")
    if a.require_platforms and not a.merge:
        p.error("--require-platforms is for --merge")
    try:
        if a.merge:
            if a.version and any(_read_manifest(path)[0]["version"] != a.version for path in a.merge):
                raise ValueError("Merged package version does not match --version")
            manifest, archives = merge(a.merge, a.out, a.require_platforms)
        else:
            manifest, archive = package(a.build_dir, a.version or DEFAULT_VERSION, a.platform, a.out)
            archives = [archive]
        manifests = [manifest]
        if a.root_manifest:
            manifests.append(write_root_manifest(manifest))
        if a.sign_tool:
            for item in manifests:
                subprocess.run([str(a.sign_tool), "sign", str(a.sign_key), str(item)], check=True)
        print(manifest)
        for archive in archives:
            print(archive)
        if not a.sign_tool:
            print("Unsigned package; Hailstone shows this during installation.")
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        p.exit(1, f"Package failed: {error}\n")


if __name__ == "__main__":
    main()
