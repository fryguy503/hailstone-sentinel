# UI checks and visual previews

The UI uses Hailstone's public Lua window API. These tools check the shipped Lua
without starting a game session or using any game art or real character data.

Run the behavioral check against both packs with a local Lua 5.4 interpreter:

```sh
lua tools/check_ui.lua ui/modern/sentinel.lua
lua tools/check_ui.lua ui/gamepad/sentinel.lua
```

The checker navigates editing, cancellation, previews, presets, history, sharing,
import review, a failed export, settings, and locked/unlocked overlays. Each frame
checks balanced window and style stacks. It also exercises starter search/category
filters, a live palette change, fixed alert colors, and compact default dimensions.
Its host is mocked. Add `inherit` after the UI path to test missing theme libraries.

The optional visual renderer executes the actual UI Lua against Dear ImGui's
layout engine and rasterizes its draw lists and font atlas to PNG. It is a fixture
preview: native Hailstone integration, live data, audio, and physical controller
acceptance still require a client session.

Provide existing Dear ImGui v1.91.9, stb, and Lua 5.4 development files. This tool
does not download dependencies:

```sh
cmake -S tools/preview -B build-preview \
  -DIMGUI_SOURCE_DIR=/path/to/imgui \
  -DSTB_INCLUDE_DIR=/path/to/stb
cmake --build build-preview --config Release
build-preview/ui_preview ui/modern/sentinel.lua tools/preview_fixture.lua library.png library
```

On Windows the executable may be in `build-preview/Release/`; Lua's runtime DLL
must be on `PATH`. Windows previews use the installed Segoe UI font; other systems
use Dear ImGui's built-in font. The client chooses its own fonts.

The renderer uses the core's actual 25-event catalog and 32 starter presets. To
preview the active pack's real theme, point `HAILSTONE_UI_DIR` at an existing
Hailstone `ui/modern` or `ui/gamepad` directory. `HAILSTONE_PALETTE` selects a palette
(for example `midnight` or `daylight`). `tools/preview_theme.lua` routes nested
theme-library loads to that pack; it copies no client theme code into the addon.
A requested theme that fails to load stops rendering instead of producing a
misleading fallback screenshot.

Optional `HAILSTONE_UI_SCALE=1.25` checks larger interface text and geometry.
`SENTINEL_PREVIEW_WINDOW=860x620` checks a user-resized main window. These environment
variables are used only by the development preview tool.

Available preview modes: `library`, `editor`, `presets`, `sharing`, `settings`,
`activity`, and `overlay`. Append display width and height to check a smaller
screen, for example `library 1000 780`. Run with `ui/gamepad/sentinel.lua` to check
the Gamepad pack's window.
