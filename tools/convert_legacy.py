#!/usr/bin/env python3
"""Convert a Citadel [Sentinel] INI section to a standalone Sentinel export.

Usage: python tools/convert_legacy.py "Character.ini" --out "sentinel-alerts.json"
The input is read-only. The output must be a new file. Import the resulting text
in Sentinel's Sharing page, review it, then enable the alerts you want to use.
Custom WAV files are referenced by filename and must be copied separately.
"""
# SPDX-License-Identifier: MIT
from __future__ import annotations

import argparse
import configparser
import json
from pathlib import Path
import sys

MAX_INPUT = 1024 * 1024
MAX_EXPORT = 128 * 1024
EVENTS = (
    ("health", "health_below", "My health is low"),
    ("mana", "mana_below", "My mana is low"),
    ("endurance", "endurance_below", "My endurance is low"),
    ("playerhealth", "player_health_below", "A player's health is low"),
    ("playermana", "player_mana_below", "A player's mana is low"),
    ("pethealth", "pet_health_below", "My pet's health is low"),
    ("targethealth", "target_health_below", "My target's health is low"),
    ("grouphealth", "group_health_below", "A group member's health is low"),
    ("buff", "buff_fading", "A buff is about to fade"),
    ("buffmissing", "buff_missing", "A required buff is missing"),
    ("spawn", "spawn_appears", "An NPC appears nearby"),
    ("player", "player_appears", "A player appears nearby"),
    ("cast", "target_cast", "My target begins casting"),
    ("tell", "incoming_tell", "I receive a tell"),
    ("raidmessage", "raid_message", "I receive raid chat"),
    ("guildmessage", "guild_message", "I receive guild chat"),
    ("fellowshipmessage", "fellowship_message", "I receive fellowship chat"),
    ("missednote", "missed_note_or_fizzle", "I fizzle or miss a note"),
    ("chattext", "chat_text_match", "Chat matches a pattern"),
    ("invisibilityfading", "invisibility_fading", "My invisibility starts fading"),
    ("groupmessage", "group_message", "I receive group chat"),
    ("spellresisted", "spell_resisted", "My spell is resisted"),
    ("petlost", "pet_lost", "My pet disappears"),
)
COLORS = ("red", "orange", "yellow", "green", "cyan", "blue", "purple", "white")
TONES = ("chime", "alert", "critical", "question", "default")
SUBJECT_REQUIRED = {
    "player_health_below", "player_mana_below", "buff_fading", "buff_missing",
    "spawn_appears", "player_appears", "chat_text_match",
}


class ConversionError(ValueError):
    """An input cannot be converted without changing its meaning."""


def decode_ini(data: bytes) -> str:
    if len(data) > MAX_INPUT:
        raise ConversionError("The input exceeds the 1 MiB limit.")
    try:
        if data.startswith((b"\xff\xfe", b"\xfe\xff")):
            text = data.decode("utf-16")
        else:
            try:
                text = data.decode("utf-8-sig")
            except UnicodeDecodeError:
                text = data.decode("cp1252")
    except UnicodeError as error:
        raise ConversionError("The INI is not valid UTF-8, UTF-16, or Windows-1252 text.") from error
    if "\x00" in text:
        raise ConversionError("The input contains binary data.")
    return text


def convert_text(text: str) -> dict:
    # Win32 profile keys are case-insensitive, duplicate keys use the last value,
    # and percent signs in notification templates must remain literal.
    parser = configparser.ConfigParser(interpolation=None, strict=False, allow_no_value=True)
    try:
        parser.read_string(text)
    except configparser.Error as error:
        raise ConversionError("The input is not a readable Windows INI file.") from error
    matches = [name for name in parser.sections() if name.casefold() == "sentinel"]
    if not matches:
        raise ConversionError("The INI has no [Sentinel] section.")
    section = parser[matches[-1]]

    def get(key: str, default: str = "", maximum: int = 512) -> str:
        value = section.get(key, default)
        value = "" if value is None else value.strip()
        if len(value) >= 2 and value[0] == value[-1] and value[0] in "\"'":
            value = value[1:-1]
        if len(value.encode("utf-8")) > maximum:
            raise ConversionError(f"{key} exceeds the standalone field limit ({maximum} UTF-8 bytes).")
        if any(ord(char) < 32 for char in value):
            raise ConversionError(f"{key} contains unsupported control characters.")
        return value

    def integer(key: str, default: int, low: int, high: int) -> int:
        raw = get(key, str(default), 32)
        try:
            value = int(raw, 10)
        except ValueError as error:
            raise ConversionError(f"{key} must be a whole number.") from error
        if not low <= value <= high:
            raise ConversionError(f"{key} must be between {low} and {high}.")
        return value

    count = integer("Count", 0, 0, 64)
    if count == 0:
        raise ConversionError("The [Sentinel] section contains no alerts to convert.")
    alerts = []
    for index in range(count):
        prefix = f"Alert{index}"
        token = get(prefix + "Event", "health", 48).casefold()
        definition = next((item for item in EVENTS if token in (item[0], item[1])), None)
        if definition is None and token.isdecimal() and int(token) < len(EVENTS):
            definition = EVENTS[int(token)]
        if definition is None:
            raise ConversionError(f"{prefix}Event has an unsupported event: {token!r}.")
        _, event, label = definition
        subject = get(prefix + "Subject")
        if event in SUBJECT_REQUIRED and not subject:
            raise ConversionError(f"{prefix}Subject is required for {label.lower()}.")
        outputs = integer(prefix + "Outputs", 7, 1, 15)
        threshold = integer(prefix + "Threshold", 25, 0, 36000)
        if event.endswith("_below") and not 1 <= threshold <= 100:
            raise ConversionError(f"{prefix}Threshold must be 1 to 100 percent.")
        if event == "buff_fading" and not 1 <= threshold <= 36000:
            raise ConversionError(f"{prefix}Threshold must be 1 to 36000 seconds.")
        if event in ("spawn_appears", "player_appears") and not 1 <= threshold <= 10000:
            raise ConversionError(f"{prefix}Threshold needs a positive range from 1 to 10000 units; unlimited legacy range is unsupported.")
        sound_file = get(prefix + "SoundFile", maximum=96)
        if sound_file and (sound_file.startswith(".") or ".." in sound_file or
                           any(char in sound_file for char in "/\\:") or not sound_file.lower().endswith(".wav")):
            raise ConversionError(f"{prefix}SoundFile must be a WAV filename without a folder or drive.")
        message = get(prefix + "Message")
        alert = {
            "id": f"legacy-{index + 1}", "name": message[:96] if message else label,
            "event": event, "enabled": bool(integer(prefix + "Enabled", 1, 0, 1)),
            "category": get(prefix + "Category", "General", 48) or "General",
            "section": get(prefix + "Section", "General", 64) or "General",
            "subsection": get(prefix + "Subsection", maximum=64),
            "zone_id": integer(prefix + "ZoneId", 0, 0, 65535), "subject": subject,
            "threshold": threshold, "message": message,
            "cooldown": integer(prefix + "Cooldown", 10, 0, 3600),
            "chat": bool(outputs & 1), "overlay": bool(outputs & 2),
            "sound": bool(outputs & 4), "speech": bool(outputs & 8),
            "volume": integer(prefix + "SoundVolume", 100, 0, 100),
            "tone": TONES[integer(prefix + "Sound", 0, 0, len(TONES) - 1)],
            "sound_file": sound_file, "speech_text": get(prefix + "SpeechText"),
            "voice": integer(prefix + "SpeechVoice", 0, 0, 1),
            "rate": integer(prefix + "SpeechRate", 0, -10, 10),
            "color": COLORS[integer(prefix + "TextColor", 0, 0, len(COLORS) - 1)],
            "size": integer(prefix + "TextSize", 1, 0, 4),
            "duration": integer(prefix + "ScreenDuration", 3000, 500, 30000) / 1000,
        }
        alerts.append(alert)
    return {"format": "hailstone-sentinel", "version": 1, "alerts": alerts}


def convert_file(source: Path, destination: Path) -> int:
    if source.resolve() == destination.resolve():
        raise ConversionError("Input and output must be different files. The legacy INI is read-only.")
    with source.open("rb") as stream:
        data = stream.read(MAX_INPUT + 1)
    result = convert_text(decode_ini(data))
    text = json.dumps(result, indent=2, ensure_ascii=False) + "\n"
    if len(text.encode("utf-8")) > MAX_EXPORT:
        raise ConversionError("The converted export exceeds 128 KiB. Convert a smaller copy of the legacy section.")
    # Exclusive creation also protects existing files reached through hardlinks
    # or symlinks. Never replace the user's source or an existing export.
    with destination.open("x", encoding="utf-8", newline="\n") as stream:
        stream.write(text)
    return len(result["alerts"])


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("input", type=Path, help="Legacy Citadel character INI (read-only)")
    parser.add_argument("--out", type=Path, required=True, help="New Sentinel JSON export; existing files are never overwritten")
    args = parser.parse_args(argv)
    try:
        count = convert_file(args.input, args.out)
    except (ConversionError, OSError) as error:
        print(f"Conversion failed: {error}", file=sys.stderr)
        return 1
    print(f"Converted {count} alert(s) to {args.out}.")
    print("Import the JSON in Sentinel's Sharing page. Imported alerts start disabled for review.")
    print("Custom WAV files are not copied; add them to the new character's Sentinel Sounds folder separately.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
