# Building Sentinel

Requires CMake 3.24+, a C++20 compiler, and the public Hailstone SDK. A snapshot of the MIT-licensed SDK is included; no client internals or game assets are build dependencies. JSON for Modern C++ is vendored, so the normal build downloads nothing.

From a Visual Studio developer terminal on Windows:

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_TRY_COMPILE_CONFIGURATION=Release
cmake --build build
ctest --test-dir build --output-on-failure
lua tools/check_ui.lua
python -m unittest discover -s tests -p "test_*.py"
python tools/package.py --build-dir build --version 1.0.0-preview.7 --platform windows-x64 --out release/windows-x64
```

The Release DLL uses the static MSVC runtime. The output contains a deterministic ZIP and its platform manifest. Do not commit build output or private signing keys. Optional `--sign-tool <hailstone_sign>` and `--sign-key <existing-secret-file>` sign the final manifests; keep using the same key after players pin it.

For another SDK snapshot, set `-DHAILSTONE_SDK_DIR=/path/to/hailstone/sdk`. Build against the same ABI as the client. The included header is ABI 3, from Hailstone commit `9f6226a86982380440da561cf80dbc2e86868674`.

Linux x64 uses the same CMake build and CTest commands; `-G Ninja` is optional. Release CI uses Ubuntu 22.04. The bundled miniaudio backend dynamically discovers system audio libraries, so there is no extra addon audio library to install. Optional speech requires a system `espeak-ng` or `espeak` executable.

For macOS Apple Silicon, use an arm64 native runner or Mac with Xcode command-line tools:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=13.3
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure
codesign --force --sign - --timestamp=none build/sentinel.dylib
codesign --verify --strict build/sentinel.dylib
python3 tools/package.py --build-dir build --version 1.0.0-preview.7 --platform macos-arm64 --out release/macos-arm64
```

Mac playback uses CoreAudio through miniaudio; native speech uses AVFoundation. No third-party speech program is needed. The normal smoke test stays silent. Set `SENTINEL_TEST_SPEECH=1` only for an explicit audible speech check on a real Mac. The distributed library is ad-hoc signed, not Developer ID signed or notarized.

The read-only CI workflow builds/tests all three platforms, verifies architectures, signs the Mac library, and uploads one artifact per platform. Download those artifacts without modifying the ZIPs, then produce one combined manifest:

```sh
python3 tools/package.py --merge downloaded/windows/hailstone-addon.txt downloaded/linux/hailstone-addon.txt downloaded/macos/hailstone-addon.txt --version 1.0.0-preview.7 --out release/v1.0.0-preview.7 --root-manifest
```

Merge verifies matching metadata/shared files, all three expected platforms, ZIP paths, exact sizes, and SHA-256 values. It preserves ZIP bytes, including Mac signatures. `--root-manifest` writes absolute release URLs so the repository link works for prereleases. Publish the combined manifest and all three ZIPs together.

## Layout

- `src/engine.*`: platform-independent rules, normalized configuration, text matching, templates, sharing, presets.
- `src/plugin.cpp`: the public SDK adapter, character settings, commands, bounded HUD/activity queues, and Lua UI functions.
- `src/audio.*`, `src/audio_data.*`, `src/speech_*`: bounded worker-owned sound and speech, portable PCM handling, and native platform adapters. No worker calls the client host API.
- `ui/{modern,gamepad}/sentinel.lua`: the addon windows for both UI packs.
- `tests/`: deterministic rules and a fake public SDK host; no server or game installation required.
- `tools/`: deterministic packaging, DLL-settings migration, UI checks, optional rendered preview.

Settings live under the host's character configuration directory. Writes use a temporary file followed by replacement; an unreadable original blocks saving and is left intact. Imports are capped at 128 KiB, 128 alerts, and 12 JSON nesting levels. Libraries hold at most 128 rules; settings files are bounded separately at 1 MiB. Chat patterns, matching work, audio requests, history, and overlays are bounded.

Window visibility is an installation-wide preference in `ui-settings.json` under
the host's plugin configuration directory. The first load opens the window and
records that state; subsequent explicit show/hide actions save immediately.
Character settings loads and zone callbacks do not change visibility. This file
is player data and must never be included in the addon ZIP or shipped file list.

## Release checks

Build Release, run CTest and the Lua/Python checks, render the actual Lua UI, then package. Verify the ZIP contents, its size/hash against the manifest, and the DLL exports and runtime dependencies. Use the client's official `tools/make_addon.py` as an independent package validator when a Hailstone checkout is available. Publish the ZIP and manifest together. The repository-level manifest must describe the same package.

Preview releases remain appropriate until a player has checked live alerts, volume, speech, zoning, reloads, and both UI packs in Hailstone.
