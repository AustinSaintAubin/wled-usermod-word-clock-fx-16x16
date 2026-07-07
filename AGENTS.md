# Agent working notes — Word Clock FX (WLED usermod)

Operational knowledge for AI agents / contributors working in this repo. End-user docs
(features, grammar, pinout, install) live in [readme.md](readme.md) — don't duplicate them here.

## What this is

An **out-of-tree WLED community usermod**: a first-class WLED Effect ("Word Clock FX") for
RGBW matrix word clocks, with selectable layouts (16×16 exact-minute default, 11×10
"WordClock 2022" 5-minute, custom from `/wordclock.json`), period-of-day / AM-PM and
temperature words, an optional Open-Meteo weather client (weather → preset switching),
corner-button → LED feedback and corner-LED minute dots. **All code lives in one file:
`usermod_word_clock_fx.cpp`** (~1100 lines). MIT licensed.

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

Building requires a **local WLED checkout**, assumed below at `../WLED` (a sibling of this
repo) — adjust paths if yours lives elsewhere. The WLED checkout's `platformio_override.ini`
(gitignored, so it may not exist yet) holds the build env: if missing, create it from
[examples/platformio_override.sample.ini](examples/platformio_override.sample.ini) (copy to the
WLED repo root as `platformio_override.ini`), which defines envs `esp32dev_wordclock_16x16` and
`esp32dev_wordclock_16x16_ota`.

### Build & verify loop (do this for every code change)

`pio` is the PlatformIO CLI (a VSCode PlatformIO install puts it at `~/.platformio/penv/bin/pio`).
The `-d` flag points it at the WLED checkout so the commands work from any directory.

1. In `../WLED/platformio_override.ini`, point `custom_usermods` at your **local checkout**
   instead of the git URL: comment out the
   `https://github.com/AustinSaintAubin/wled-usermod-word-clock-fx-16x16.git#main` line and
   enable (or add) this one in the same multiline block — `symlink://` requires an **absolute**
   path:
   `symlink:///absolute/path/to/wled-usermod-word-clock-fx-16x16`
2. Build: `pio run -d ../WLED -e esp32dev_wordclock_16x16`
   - Success = `[SUCCESS]`, ~83% flash, and the mod named in
     `INFO: Code from usermod libraries found in binary: … wled-usermod-word-clock-fx-16x16`.
3. **Restore the git-URL line** (and re-comment the symlink) in the override before finishing.
4. A clean compile does **not** prove the settings UI works (see "Settings-UI quirks" below) —
   if you touched `appendConfigData()`, the user must load the usermod settings page in a
   browser to confirm.

If the existing `platformio_override.ini` is unrecognizable, don't guess — rebuild a minimal one
from the sample above, or ask the user.

Gotchas:
- PlatformIO **caches** the git-fetched mod. After pushing to `main`, force a re-pull with
  `rm -rf ../WLED/.pio/libdeps/*/wled-usermod-word-clock-fx-16x16` before rebuilding.
- Flashing the device: `pio run -d ../WLED -e esp32dev_wordclock_16x16_ota -t upload`
  (OTA; the target host comes from `upload_port` in the override) — **only when the user asks**;
  it changes their hardware.
- If the device's Info panel shows an old version, it's running old firmware / a cached libdep —
  not necessarily a code bug.

## Architecture map (`usermod_word_clock_fx.cpp`)

In file order:
- **Layouts + `wcfxBuildMask()`** — a layout (`WcfxLayout`) = dims + grammar id + a role-tagged
  word table (`WcfxLayoutWord {role,x,y,len}`, roles in `WcfxRole`: WR_IT…WR_HOT, WR_M1..M20/M25,
  WR_H1..H12). Built-in tables (`wcWords16`, `wcWords11`) are **PROGMEM — access only via
  `wcfxWordAt()`/`memcpy_P`**. Two grammar engines (exact-minute / floored 5-minute) drive any
  layout via `wcfxLightRole()`; roles a layout lacks are silent no-ops. Mask is
  `WcfxRow(uint32_t)[WCFX_MAX_H]` (max 32×32). Works in logical X/Y only (serpentine handled by
  WLED 2D cfg); the layout draws from the segment's top-left (position via segment 2D bounds).
  The active layout is the global `wcfx_layout`; bump `wcfx_layoutGen` after changing it.
- **`mode_word_clock_fx()`** — the effect; per-segment state `WCFXRt` in `SEGENV.data`
  (`allocateData`), crossfades via `strip.getTransition()`, rebuilds on minute/band/layout-gen
  change.
- **Custom layouts** — `loadCustomLayout()` parses `/wordclock.json` (own `DynamicJsonDocument`,
  never WLED's pinned doc) into a heap table; on any error it falls back to the 16×16 **before**
  freeing the old table (no dangling `wcfx_layout`). Status string surfaces on the Info page;
  `{"WordClockFx":{"reloadLayout":true}}` re-reads without reboot.
- **`WxState` enum + `codeToState()`** — WMO code → weather state. `WX_SEVERE` is
  **intentionally never produced here** (Open-Meteo has no tornado code); it's pushed externally
  via JSON API / Home Assistant. Keep the comment that says so.
- **Open-Meteo client** — `fetch()` / `fetchOpenMeteo()` / `geocode()`. **Plain HTTP only**:
  TLS is disabled in the WLED framework build (`CONFIG_MBEDTLS_TLS_DISABLED=y`); do not attempt
  HTTPS/WiFiClientSecure/esp_http_client. 2000 ms timeout caps `loop()` blocking; `haveHumidity`/
  `haveWind` reset per response.
- **`handleOverlayDraw()`** — corner-LED minute dots (`minute % 5` on `cbLed[]`, gated on the
  effect being active) drawn first, then corner-button → LED feedback using native WLED buttons
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
4. `gh release create vX.Y.Z --title vX.Y.Z --notes "…"` (gh must be authed as the repo owner).

Docs-only changes (like this file): commit + push, no version bump.

### Maintainer environment notes (Austin's workstation — ignore on other machines)

- WLED checkout: `/home/austin.st.aubin/Documents/PlatformIO/WLED`; its personal override uses
  env `esp32dev_lolin32_wordclock_16x16` (+ `_ota`) instead of the sample's names; `pio` lives at
  `~/.platformio/penv/bin/pio`.
- OTA target: `wordclock01.internal`.
- A private NAS mirror is configured as git remote `nas` — after any push to GitHub, also run
  `git push nas main --tags`.
