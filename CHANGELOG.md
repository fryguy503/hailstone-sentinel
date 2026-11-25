# Changelog

## 1.0.0-preview.6

- Add **Timer bars** to Modern and Gamepad. Save up to 24 named timers per character, each lasting 1 to 86,400 seconds, and start, restart, or stop them manually.
- Move, resize, and minimize individual countdown overlays or group them in one movable, resizable, collapsible pane. Lock the overlays to let game clicks pass through.
- Choose a starting color and optional warning and critical colors based on seconds remaining. Adjust each timer's opacity and the grouped pane's opacity from 10% to 100%.
- Add **Start or restart a timer** as an alert result, with support for timer-only rules and combinations with the existing outputs.
- Add **A timer bar reaches its threshold** as an alert event. Set the seconds remaining to `0` to alert when the selected timer ends.
- Save timer definitions and layout mode per server and character, with separate position and size settings for individual bars and the grouped pane. Active countdowns continue across zoning and Master Pause, and reset when switching characters or reloading Sentinel. Master Pause suppresses automatic rules, including their timer starts.
- Clear timer selections on imports for re-selection, disable linked rules when a timer is deleted, and reject timer rule loops.

## 1.0.0-preview.5

- Add **I receive a buff** and **I receive a debuff** events, with an optional exact spell-name filter and starter presets.
- Include new effect names in automatic messages and `{L}` templates; summarize multiple arrivals in one notification per rule.
- Keep existing effects quiet on login, zoning, enabling or editing a rule, and timer refreshes. Reapplication alerts after an observed absence; cooldown-suppressed arrivals are not replayed.
- Ignore unknown spell types and incomplete buff reads. Use stable SDK slot reads across client revisions.
- Keep native Windows x64, Linux x64, and macOS Apple Silicon packages.

## 1.0.0-preview.4

- Add native Linux x64 and macOS Apple Silicon addon packages alongside Windows x64.
- Support built-in tones and custom WAV playback on Linux and Mac, native Mac speech, and optional Linux speech through espeak-ng/espeak.
- Build/test all three platforms in CI and publish one manifest that selects the matching package.
- Verify and ad-hoc sign the arm64 Mac library. Preserve existing alert settings and remembered window visibility.

## 1.0.0-preview.3

- Open Sentinel on first use, then remember the last open or closed state across zoning, plugin reloads, restarts, and character changes.
- Save window visibility separately from character alert libraries, including when closing before creating any alerts.

## 1.0.0-preview.2

- Follow the active client theme in Modern and Gamepad, including live palette changes.
- Replace tall starter cards with a searchable, category-filtered list and a selected preset detail pane.
- Reduce the default window size, header, spacing, and control padding while preserving resizing and UI scaling.

## 1.0.0-preview.1

- First standalone Hailstone addon, independent of the custom DLL.
- 23 trigger types, starter presets, guided editor, searchable library, activity history, and reviewed sharing.
- Adjustable banners, Windows tones/WAV audio and speech, Quiet mode, and master pause.
- Character-specific settings, bounded matching, safe imports, and legacy INI conversion.
- Modern and Gamepad UI-pack windows; Windows x64 preview package.
- Automated core, native-adapter, migration, packaging, and UI checks. Live gameplay and audio acceptance remain pending.
