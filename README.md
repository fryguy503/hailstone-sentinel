# Sentinel for Hailstone

**A calmer way to keep watch.** Sentinel turns the moments you care about into clear, personal alerts: low health, a fading buff, a lost pet, a dangerous cast, or a message you do not want to miss.

It is a standalone addon. Your custom DLL is not required.

![Sentinel alert library](docs/sentinel-preview.png)

Starter kits now use a compact, searchable list:

![Compact starter kits](docs/starter-kits.png)

## Get started

1. Open **Add-ons** on Hailstone's login screen.
2. Paste `https://github.com/fryguy503/hailstone-sentinel`, inspect the package, and install it.
3. Enter the world and load the plugin with `/plugin sentinel`.
4. Open **Sentinel alerts** from the game menu, or type `/sentinel`.
5. Choose **Starter kits**, customize an alert, and press **Preview** before saving.

The first package is a **Windows x64 preview**, for Hailstone **0.7.0-beta or newer** using SDK ABI 3. It is unsigned; Hailstone identifies this during installation. If Hailstone cannot find a preview release automatically, paste that release's tag link from the repository's Releases page. Use Hailstone's Plugins controls to keep the plugin enabled for your character.

## Make it yours

- **23 event types.** Health, mana, endurance, party and pet warnings, buffs, nearby spawns, target casts, chat channels, custom text patterns, fizzles, resists, and invisibility warnings.
- **A guided rule builder.** Choose a trigger, pick notification styles, and read a plain-language summary before saving.
- **A familiar, compact window.** Follows your active client theme and palette. Starter kits use a searchable list with details for the selected preset.
- **A searchable library.** Organize up to 128 alerts with categories, sections, subsections, and optional zone restrictions.
- **Notifications that fit the moment.** Colored banners in five sizes, local chat, five built-in chimes, custom WAV files, and Windows system speech.
- **A comfortable HUD.** Move the banner stack, lock it in place, or mute audio with Quiet mode. Master Pause stops automatic alerts while preserving your library.
- **Activity history.** Review the last 100 notifications, with previews clearly marked.
- **Reviewed sharing.** Export one rule or the library as text. Inspect incoming rules before adding them; imports always begin disabled.
- **Separate character settings.** Each server and character has its own library. Nothing is written to your game assets folder.

Sentinel only observes and notifies. It does not cast, target, move, send chat to other players, or change game mechanics. Its declared permissions are `read,commands,events,ui`.

## A few examples

| I want to know when… | Start with… |
| --- | --- |
| My health reaches 15% | Emergency heal |
| A group member reaches 35% health | Group emergency |
| My buff has 30 seconds left | Refresh a buff |
| My pet disappears or charm breaks | Pet lost or charm broke |
| My target begins casting | Interrupt a spell |
| Someone sends me a tell | Never miss a tell |
| A chat line reports a large hit | Custom text pattern |

Percentage rules alert once when the threshold is reached. They rearm after recovery above the threshold plus three percentage points. A cooldown limits notifications; it does not repeat an alert while a condition remains true. Group members and targets are tracked independently. Logging in without a pet does not trigger a lost-pet warning.

## Text patterns and messages

Chat patterns match the **whole line**, ignoring ASCII letter case. Add `*` around text to find it anywhere.

| Pattern | Meaning |
| --- | --- |
| `*rampage*` | Any line containing rampage |
| `{S1} hits {C} for {N1>=1000} points of damage.` | Capture the attacker and damage, only for hits of 1,000 or more |
| `*{S1} tells you, '{S2}'` | Capture text from a tell-shaped line |

`*` matches any text, `?` matches one character, `{S1}` captures text, and `{N1}` captures a number. Numeric captures accept `=`, `!=`, `<`, `<=`, `>`, and `>=` comparisons, signed numbers, decimals, and comma separators.

In messages or speech, use `{C}` for your character, `{L}` for the matching line or state description, `{COUNTER}` for the rule's firing count, and captures such as `{S1}` and `{N1}`. Leave the message empty for an automatic description.

## Sounds, speech, and sharing

Place custom WAV files in **Sounds beside your character's Sentinel `config.json`**, then click **Rescan sounds**. Settings & help shows the exact settings path. Supported WAVs are PCM 16-bit, mono or stereo, 8–96 kHz, at most 30 seconds and 16 MiB. An unreadable WAV falls back to a built-in tone. Each rule has its own volume.

Speech uses up to two installed Windows SAPI voices, with adjustable rate and optional separate wording. Linux and macOS source builds currently provide visual and chat alerts; native audio backends for those systems are not implemented.

In **Share & import**, select export text and use Ctrl+C. Paste a shared collection with Ctrl+V, choose **Review import**, and add the reviewed alerts. Enable each imported rule after checking it. Sharing uses ordinary text and does not require Bastion's server-specific raid-sharing protocol. Imports contain data only and cannot run commands or code.

Existing DLL users can convert a **copy** of their Citadel character INI with [the migration tool](tools/convert_legacy.py), then review its JSON through Share & import:

```powershell
python tools/convert_legacy.py path/to/CitadelCharacter.ini --out sentinel-import.json
```

## Commands

| Command | Action |
| --- | --- |
| `/sentinel` | Open Sentinel |
| `/sentinel on`, `off`, `toggle` | Control all automatic alerts |
| `/sentinel hide` | Close the library |
| `/sentinel list` | List your alerts and their numbers |
| `/sentinel test 2` | Preview alert number 2 |

## What to expect from the preview

The addon has automated rule, SDK adapter, persistence, import, and UI checks, plus rendered UI verification. **Live gameplay and physical audio acceptance are still needed.**

Sentinel uses information the server supplies. Other players' mana may be unavailable; absent readings are ignored. Group checks cover members present in your zone. Target casts are sampled every 100 ms, so a very short cast may be missed. Channel alerts recognize the standard English chat envelopes because Hailstone 0.7 passes RGB rather than channel metadata to native chat callbacks. Custom patterns can be used for localized text.

The DLL's binary raid-sharing packets and INI files are not read automatically. The migration tool creates a portable import and leaves the source untouched. Settings that cannot be read are preserved and reported instead of overwritten.

For building, packaging, testing, and architecture, see [Building](docs/BUILDING.md) and [Validation](docs/VALIDATION.md). MIT licensed; dependency notices are in [THIRD_PARTY.md](THIRD_PARTY.md).
