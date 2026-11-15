# Preview validation

The initial Windows x64 preview was built locally with MSVC in Release mode. The standalone DLL uses the public Hailstone SDK only, with the static C++ runtime. Its runtime imports are Windows system libraries.

## Automated coverage

- Core rules: threshold edges and three-point hysteresis, invalid data, cooldowns, independent members/targets, pet baseline/loss, buffs, nearby entities, cast observations, full-line patterns, numeric comparisons, captures, templates, bounded input and reviewed sharing.
- Fake native host: public SDK callbacks, UI serialization, commands, edits, duplication/deletion, settings reload, separate characters, disabled imports, import rejection without partial mutation, recursion, zoning, mapped remote mana and casts, compact buff/song indexes, and preserving unreadable settings.
- UI: both packs compile as Lua; scripted traversal covers editing/cancel/preview, presets, activity, sharing/review/import, failed export, comfort settings, and locked/unlocked overlays. Window and style stacks balance after each frame.
- Python: migration fixtures and deterministic archive/manifest integrity.
- Package: ZIP shape, Lua syntax, manifest size/SHA-256, and independent validation against Hailstone's official addon packaging rules.

## Visual checks

The development preview runs the shipped Lua against real Dear ImGui layout and draw lists, then rasterizes them to PNG. Library, editor, presets, sharing, settings, activity, HUD, and a compact Gamepad-pack layout were inspected. Preview data is invented and contains no game assets or account data. This verifies the addon layout; Hailstone chooses its own fonts and UI scale.

Preview 2 additionally loads the client's actual Modern and Gamepad theme libraries,
checks dark and daylight palettes, and renders all 30 real starter presets in the
compact list. A saved 860×620 window and Modern UI scale 1.25 were visually checked.
The Lua checks cover starter search/category filtering, live palette changes,
preservation of alert colors, fallback styling, and the smaller default window.

## Still to accept in a live client

- Installing/loading from the Add-ons and Plugins screens.
- Actual health/buff/pet/cast and channel events on a server.
- Audible built-in and custom sounds, installed SAPI voices, rate, volume, quiet mode, and interrupting queues.
- Zoning, character switching, UI reload, and overlay positioning with real screen scale and input devices.
- Physical controller use of the Gamepad pack.

Local tests and fixture screenshots do not establish live gameplay or hardware acceptance. Linux CI checks portability of source and tests; this release packages Windows x64 only.
