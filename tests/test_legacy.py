"""Synthetic legacy migration checks; no player files are read."""
# SPDX-License-Identifier: MIT
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

SPEC = importlib.util.spec_from_file_location("convert_legacy", Path(__file__).parents[1] / "tools" / "convert_legacy.py")
legacy = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(legacy)


class LegacyConversionTest(unittest.TestCase):
    def test_all_original_events_and_numeric_order(self):
        lines = ["[Sentinel]", "Count=46"]
        for index, (token, event, _) in enumerate(legacy.EVENTS):
            for offset, value in ((index, token), (index + 23, str(index))):
                lines += [f"Alert{offset}Event={value}", f"Alert{offset}Subject=Example", f"Alert{offset}Threshold=30"]
        alerts = legacy.convert_text("\n".join(lines))["alerts"]
        self.assertEqual(len(alerts), 46)
        for index, (_, event, _) in enumerate(legacy.EVENTS):
            self.assertEqual(alerts[index]["event"], event)
            self.assertEqual(alerts[index + 23]["event"], event)

    def test_preserves_settings_and_templates(self):
        result = legacy.convert_text("""[Other]
Unrelated=private
[Sentinel]
Enabled=0
Count=1
Alert0Enabled=0
Alert0Event=CHATText
Alert0Subject={S1} hits {C} for {N1>=1000} damage*
Alert0Category=Raids
Alert0Section=Example raid
Alert0Subsection=Phase 2
Alert0ZoneId=406
Alert0Message={S1}: {N1} on {C} at 30%
Alert0Threshold=0
Alert0Outputs=10
Alert0Cooldown=5
Alert0TextSize=4
Alert0TextColor=6
Alert0ScreenDuration=4500
Alert0Sound=2
Alert0SoundFile=warning.wav
Alert0SoundVolume=45
Alert0SpeechText=Watch {C}
Alert0SpeechVoice=1
Alert0SpeechRate=-3
""")
        self.assertEqual(result["format"], "hailstone-sentinel")
        self.assertEqual(result["version"], 1)
        a = result["alerts"][0]
        self.assertFalse(a["enabled"])
        self.assertEqual((a["category"], a["section"], a["subsection"], a["zone_id"]), ("Raids", "Example raid", "Phase 2", 406))
        self.assertEqual(a["subject"], "{S1} hits {C} for {N1>=1000} damage*")
        self.assertEqual(a["message"], "{S1}: {N1} on {C} at 30%")
        self.assertEqual((a["chat"], a["overlay"], a["sound"], a["speech"]), (False, True, False, True))
        self.assertEqual((a["color"], a["size"], a["duration"], a["tone"], a["volume"]), ("purple", 4, 4.5, "critical", 45))
        self.assertEqual((a["speech_text"], a["voice"], a["rate"]), ("Watch {C}", 1, -3))
        self.assertNotIn("Unrelated", json.dumps(result))

    def test_missing_optional_values_use_original_defaults(self):
        a = legacy.convert_text("[sentinel]\ncount=1\n")["alerts"][0]
        self.assertEqual(a["event"], "health_below")
        self.assertEqual((a["threshold"], a["cooldown"], a["size"], a["color"], a["duration"], a["volume"]), (25, 10, 1, "red", 3, 100))
        self.assertTrue(a["enabled"] and a["chat"] and a["overlay"] and a["sound"])
        self.assertFalse(a["speech"])

    def test_encodings(self):
        text = "[Sentinel]\nCount=1\nAlert0Message=Prêt à jouer\n"
        for codec in ("utf-8-sig", "utf-16", "cp1252"):
            self.assertEqual(legacy.convert_text(legacy.decode_ini(text.encode(codec)))["alerts"][0]["message"], "Prêt à jouer")

    def test_rejects_unsupported_and_invalid_values(self):
        for text in ("[Other]\nCount=1", "[Sentinel]\nCount=0", "[Sentinel]\nCount=65",
                     "[Sentinel]\nCount=1\nAlert0Event=unknown", "[Sentinel]\nCount=1\nAlert0Event=23",
                     "[Sentinel]\nCount=1\nAlert0Event=buffmissing", "[Sentinel]\nCount=1\nAlert0Outputs=16",
                     "[Sentinel]\nCount=1\nAlert0Outputs=0", "[Sentinel]\nCount=1\nAlert0Threshold=101",
                     "[Sentinel]\nCount=1\nAlert0SoundFile=../private.wav", "[Sentinel]\nCount=1\nAlert0ScreenDuration=no"):
            with self.subTest(text=text), self.assertRaises(legacy.ConversionError):
                legacy.convert_text(text)
        with self.assertRaises(legacy.ConversionError):
            legacy.decode_ini(b" " * (legacy.MAX_INPUT + 1))
        with self.assertRaises(legacy.ConversionError):
            legacy.decode_ini(b"binary\x00data")

    def test_source_preserved_and_destination_exclusive(self):
        with tempfile.TemporaryDirectory() as temp:
            source, output = Path(temp) / "legacy.ini", Path(temp) / "alerts.json"
            original = b"[Sentinel]\nCount=1\nAlert0Message=Heal me\n"
            source.write_bytes(original)
            self.assertEqual(legacy.convert_file(source, output), 1)
            self.assertEqual(source.read_bytes(), original)
            self.assertEqual(json.loads(output.read_text(encoding="utf-8"))["alerts"][0]["message"], "Heal me")
            with self.assertRaises(FileExistsError):
                legacy.convert_file(source, output)
            with self.assertRaises(legacy.ConversionError):
                legacy.convert_file(source, source)
            self.assertEqual(source.read_bytes(), original)

    def test_ini_quote_and_duplicate_semantics(self):
        a = legacy.convert_text('[Sentinel]\nCount=1\nAlert0Message="old"\nAlert0Message="new 30%"\n')["alerts"][0]
        self.assertEqual(a["message"], "new 30%")


if __name__ == "__main__":
    unittest.main()
