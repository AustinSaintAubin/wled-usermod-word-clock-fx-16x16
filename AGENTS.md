# Agent working notes — Word Clock FX (WLED usermod)

Operational knowledge for AI agents / contributors working in this repo. End-user docs
(features, grammar, pinout, install) live in [readme.md](readme.md) — don't duplicate them here.

## What this is

An **out-of-tree WLED community usermod**: a first-class WLED Effect ("Word Clock FX") for a
16×16 RGBW matrix, with English exact-minute phrasing, period-of-day and temperature words,
an optional Open-Meteo weather client (weather → preset switching), and corner-button → LED
feedback. **All code lives in one file: `usermod_word_clock_fx.cpp`** (~930 lines). MIT licensed.

It is *not* part of the WLED tree — WLED maintainers asked for it out-of-tree
(wled/WLED#5708, closed). It's listed in the WLED community-usermods index (wled/WLED-Docs#336).
Demo: https://youtu.be/wZmo6SdLq88. Sibling mod using the same pattern:
`AustinSaintAubin/wled-usermod-sensors-i2c`.

## Key identifiers — DO NOT rename

Renaming any of these breaks users' saved configs or the WLED loader:

| Identifier | Value |
|---|---|
| C++ class | `WordClockFxUsermod` |
| Config/JSON key | `WordClockFx` |
| Effect name | `Word Clock FX` |
| Code prefixes | `WCFX_` / `wcfx` (macros, globals, JS helpers, CSS classes) |
| `library.json` name | `wled-usermod-word-clock-fx-16x16` — must keep the `wled-` prefix **and** match the repo name (WLED's `load_usermods.py` auto-recognition) |

`library.json` must keep `"build": { "libArchive": false }` or `REGISTER_USERMOD()` won't link.

## How it's consumed / how to build

There is **no standalone build** — the mod compiles inside a WLED checkout. WLED pulls it via a
git URL in `custom_usermods`:

```ini
custom_usermods = https://github.com/AustinSaintAubin/wled-usermod-word-clock-fx-16x16.git#main
```

The user's WLED checkout is a **sibling** of this repo:
`/home/austin.st.aubin/Documents/PlatformIO/WLED` (this repo is
`/home/austin.st.aubin/Documents/PlatformIO/wled-usermod-word-clock-fx-16x16`). That checkout has
a **gitignored** `platformio_override.ini` already wired up with env
`esp32dev_lolin32_wordclock_16x16` (plus an `_ota` upload variant) — it is not in any repo, so
verify it exists before relying on it.

### Build & verify loop (do this for every code change)

All `pio` commands must run **in the WLED checkout** (cd there, or pass
`-d /home/austin.st.aubin/Documents/PlatformIO/WLED`).

1. In `/home/austin.st.aubin/Documents/PlatformIO/WLED/platformio_override.ini`, point
   `custom_usermods` at the **local checkout** instead of the git URL: comment out the
   `https://github.com/AustinSaintAubin/wled-usermod-word-clock-fx-16x16.git#main` line and
   enable (or add) this one in the same multiline block:
   `symlink:///home/austin.st.aubin/Documents/PlatformIO/wled-usermod-word-clock-fx-16x16`
2. Build:
   `~/.platformio/penv/bin/pio run -d /home/austin.st.aubin/Documents/PlatformIO/WLED -e esp32dev_lolin32_wordclock_16x16`
   - Success = `[SUCCESS]`, ~83% flash, and the mod named in
     `INFO: Code from usermod libraries found in binary: … wled-usermod-word-clock-fx-16x16`.
3. **Restore the git-URL line** (and re-comment the symlink) in the override before finishing.
4. A clean compile does **not** prove the settings UI works (see "Settings-UI quirks" below) —
   if you touched `appendConfigData()`, the user must load the usermod settings page in a
   browser to confirm.

If the user's `platformio_override.ini` is missing or unrecognizable, don't guess — recreate a
minimal env from [examples/platformio_override.sample.ini](examples/platformio_override.sample.ini)
(copy to the WLED repo root as `platformio_override.ini`, swap its `custom_usermods` URL for the
symlink line above), or ask the user.

Gotchas:
- PlatformIO **caches** the git-fetched mod. After pushing to `main`, force a re-pull with
  `rm -rf /home/austin.st.aubin/Documents/PlatformIO/WLED/.pio/libdeps/*/wled-usermod-word-clock-fx-16x16`
  before rebuilding.
- Flashing the device: `pio run -d … -e esp32dev_lolin32_wordclock_16x16_ota -t upload`
  (OTA to `wordclock01.internal`) — **only when the user asks**; it changes their hardware.
- If the device's Info panel shows an old version, it's running old firmware / a cached libdep —
  not necessarily a code bug.

## Architecture map (`usermod_word_clock_fx.cpp`)

In file order:
- **Word tables + `wcfxBuildMask()`** — grid geometry as `WcfxWord {x,y,len}` rows; builds a
  16×`uint16_t` bitmask from time. Works in logical X/Y only (serpentine handled by WLED 2D cfg).
- **`mode_word_clock_fx()`** — the effect; per-segment state `WCFXRt` in `SEGENV.data`
  (`allocateData`), crossfades via `strip.getTransition()`.
- **`WxState` enum + `codeToState()`** — WMO code → weather state. `WX_SEVERE` is
  **intentionally never produced here** (Open-Meteo has no tornado code); it's pushed externally
  via JSON API / Home Assistant. Keep the comment that says so.
- **Open-Meteo client** — `fetch()` / `fetchOpenMeteo()` / `geocode()`. **Plain HTTP only**:
  TLS is disabled in the WLED framework build (`CONFIG_MBEDTLS_TLS_DISABLED=y`); do not attempt
  HTTPS/WiFiClientSecure/esp_http_client. 2000 ms timeout caps `loop()` blocking; `haveHumidity`/
  `haveWind` reset per response.
- **`handleOverlayDraw()`** — corner-button → LED feedback using native WLED buttons
  (`isButtonPressed`), overriding pixels 256–259 while held.
- **`addToConfig` / `readFromConfig`** — settings; temperature bands are clamped monotonic
  (cold ≤ cool ≤ warm) on load.
- **`appendConfigData()`** — builds the whole settings UI by injecting JS/CSS via `oappend(F(…))`.
- **`WCFX_DEFAULT_TRANSITION_MS`** build flag (optional) re-applies the boot transition time.

## Settings-UI quirks (hard-won — read before touching `appendConfigData`)

- WLED renders each usermod field as `label <input><br>`. `addInfo(name, 1, html)` injects
  **between the input and its trailing `<br>`** — a cleanup IIFE after the weather status card
  removes that stray `<br>` (keep it, it's what fixes the spacing).
- All UI code is JS inside `oappend(F("…"))` C-strings. **One unbalanced quote silently breaks
  the entire settings page** (fields render raw). A successful compile does NOT prove the UI
  works — the user must check in a browser.
- Helpers defined there: `wcfxsec` (section header), `wcfxlbl` (relabel), `wcfxtbl` (move fields
  into a table), `wcfxwxTest`/`wcfxledTest` (test buttons), `wcfxrefresh`/`wcfxupd` (status panel).
- Info-panel (`/json/info` → `u`) values render via `innerHTML`, so `<a>` links work. Keep
  "Word Clock source" as a short link (`open-meteo.com` text, full query URL in `href`) — a raw
  URL overflows the panel and `&current=` gets mangled as the `&curren;` entity. The settings
  panel's "view source" regex-extracts the `href` from that value.

## JSON API

`{"WordClockFx": {…}}` accepts: `"temp": 22.5` (push °C), `"update": true` (fetch now),
`"wxtest": 1-12` (force weather state; 12 = SEVERE), `"ledtest": N` (light pixel N for 3 s).

## Release process

The user consumes `#main`, so every push is live on their next (cache-cleared) build; releases
are checkpoints. For each user-visible batch:

1. Bump the version in **6 places**: `usermod_word_clock_fx.cpp` header `Version :` + `Updated :`
   date, `WCFX_VERSION` define, `library.json` `version`, readme `#vX.Y.Z` pin example,
   `examples/platformio_override.sample.ini` header + pin comment.
2. Semver: breaking settings/config-key change → minor; fixes/UI polish → patch.
3. Build-verify (loop above), commit with a
   `Co-Authored-By: Claude <model> <noreply@anthropic.com>` trailer, push `main`.
4. `gh release create vX.Y.Z --title vX.Y.Z --notes "…"` (gh is authed as AustinSaintAubin).
5. Mirror to the NAS backup (passwordless ssh, remote already configured):
   `git push nas main --tags`.

Docs-only changes (like this file): commit + push (GitHub **and** nas), no version bump.
