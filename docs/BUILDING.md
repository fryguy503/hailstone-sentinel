# Building Sentinel

Requires CMake 3.24+, a C++20 compiler, and the public Hailstone SDK. A snapshot of the MIT-licensed SDK is included; no client internals or game assets are build dependencies. JSON for Modern C++ is vendored, so the normal build downloads nothing.

From a Visual Studio developer terminal on Windows:

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_TRY_COMPILE_CONFIGURATION=Release
cmake --build build
ctest --test-dir build --output-on-failure
lua tools/check_ui.lua
python -m unittest discover -s tests -p "test_*.py"
python tools/package.py --build-dir build --version 1.0.0-preview.2 --platform windows-x64 --out release/v1.0.0-preview.2 --root-manifest
```

The Release DLL uses the static MSVC runtime. `release/` contains the deterministic ZIP and its `hailstone-addon.txt` manifest. `--root-manifest` also writes the repository-level manifest with an absolute release asset URL, so the repository link works for preview releases. Do not commit build output or private signing keys. Optional `--sign-tool <hailstone_sign>` and `--sign-key <existing-secret-file>` sign the manifests; keep using the same key after players pin it.

For another SDK snapshot, set `-DHAILSTONE_SDK_DIR=/path/to/hailstone/sdk`. Build against the same ABI as the client. The included header is ABI 3, from Hailstone commit `9f6226a86982380440da561cf80dbc2e86868674`.

Linux and macOS can build the core, native plugin, and tests with the same CMake commands; `-G Ninja` is optional. Visual and chat outputs are portable. Only Windows has an audio backend in this version. Source builds on other platforms are not a promise of packaged or live-tested support.

## Layout

- `src/engine.*`: platform-independent rules, normalized configuration, text matching, templates, sharing, presets.
- `src/plugin.cpp`: the public SDK adapter, character settings, commands, bounded HUD/activity queues, and Lua UI functions.
- `src/audio.*`: bounded worker-owned sound and speech. No worker calls the client host API.
- `ui/{modern,gamepad}/sentinel.lua`: the addon windows for both UI packs.
- `tests/`: deterministic rules and a fake public SDK host; no server or game installation required.
- `tools/`: deterministic packaging, DLL-settings migration, UI checks, optional rendered preview.

Settings live under the host's character configuration directory. Writes use a temporary file followed by replacement; an unreadable original blocks saving and is left intact. Imports are capped at 128 KiB, 128 alerts, and 12 JSON nesting levels. Libraries hold at most 128 rules; settings files are bounded separately at 1 MiB. Chat patterns, matching work, audio requests, history, and overlays are bounded.

## Release checks

Build Release, run CTest and the Lua/Python checks, render the actual Lua UI, then package. Verify the ZIP contents, its size/hash against the manifest, and the DLL exports and runtime dependencies. Use the client's official `tools/make_addon.py` as an independent package validator when a Hailstone checkout is available. Publish the ZIP and manifest together. The repository-level manifest must describe the same package.

Preview releases remain appropriate until a player has checked live alerts, volume, speech, zoning, reloads, and both UI packs in Hailstone.
