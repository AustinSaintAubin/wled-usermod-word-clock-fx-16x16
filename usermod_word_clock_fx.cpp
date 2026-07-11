// SPDX-License-Identifier: MIT
// usermod_word_clock_fx — MIT © Austin St. Aubin
#include "wled.h"
#ifdef ESP8266
  #include <ESP8266HTTPClient.h>
#else
  #include <HTTPClient.h>
#endif

/*
 * Word Clock FX - RGBW matrix word clock as a WLED Effect (English, selectable layouts).
 *
 * Version : 1.6.1
 * Updated : 2026-07-11
 * Author  : Austin St. Aubin <austinsaintaubin@gmail.com>
 * Note    : Developed with AI assistance; validated by building against WLED.
 *
 * Unlike usermod_v2_word_clock (which is a German clock drawn as an OVERLAY in
 * handleOverlayDraw), this usermod registers the word clock as a first-class WLED
 * *Effect* ("Word Clock FX"). That means it can be transitioned/crossfaded,
 * colored, palette-mapped and stored per-preset like any other effect.
 *
 * The effect is 2D: configure the matrix in WLED's LED preferences (set the
 * serpentine/rotation there to match the physical wiring). This code works purely
 * in logical X/Y and never touches raw LED indices. Word layouts are /wcfx-*.json
 * files on the WLED filesystem (add your own via the /edit page); the stock faces
 * (source of truth: the repo's layouts/ folder, embedded at build by tools/gen_layouts.py)
 * are seeded at boot if missing (delete one to restore stock):
 *   - wcfx-16x16-mk3-until.json / -to.json (default: until) — the MK3 face prints BOTH
 *     connectors, so picking the file in the Layout dropdown picks the phrasing
 *   - wcfx-16x16-mk1.json exact-minute, the original MK1/MK2 panels
 *   - wcfx-11x10.json "WordClock 2022" (printables.com/model/311949), 5-minute + AM/PM
 * The settings dropdown lists every layout file by its "name" field, with a docs link
 * from its "link" field. The layout draws from the segment's top-left; position it with
 * the segment's 2D bounds. Corner LEDs can count the minutes a 5-minute face can't show.
 *
 * It also has an integrated Open-Meteo weather client (free, no API key) that:
 *   - feeds the outdoor temperature to the WARM/COOL/HOT/COLD words, and
 *   - applies a user-chosen preset when the weather state changes (e.g. a "rain" preset).
 * Temperature can also be pushed via the JSON API ({"WordClockFx":{"temp":N}}).
 */

#define WCFX_VERSION "1.6.1"   // usermod_word_clock_fx

// ---- Layouts --------------------------------------------------------------------
// A layout = grid dimensions + grammar style + a role-tagged word table. A word is a
// horizontal run of letters (top-left cell x,y + length) tagged with the role it plays
// in the sentence. The same role may appear more than once (multi-segment words: every
// entry with the role lights). Roles a layout lacks are silently skipped by the grammar
// engine — that's what lets one engine drive layouts with or without period-of-day,
// AM/PM or temperature words.

typedef uint32_t WcfxRow;   // one mask row, one bit per column
#define WCFX_MAX_W 32
#define WCFX_MAX_H 32

enum WcfxRole : uint8_t {
  WR_IT, WR_IS, WR_A, WR_QUARTER, WR_HALF, WR_PAST, WR_TO,   // WR_TO doubles as UNTIL
  WR_OCLOCK, WR_MINUTES, WR_AM, WR_PM,
  WR_IN, WR_THE, WR_AT, WR_MORNING, WR_AFTERNOON, WR_EVENING, WR_NIGHT,
  WR_AND, WR_COLD, WR_COOL, WR_WARM, WR_HOT,   // WR_AND = the '&' tile before a temp word
  WR_M1,                    // minute numbers: WR_M1 + (n-1) for n = 1..20
  WR_M20 = WR_M1 + 19,
  WR_M25,                   // optional dedicated TWENTYFIVE tile (else TWENTY + FIVE)
  WR_H1,                    // hours: WR_H1 + (h-1) for h = 1..12
  WR_H12 = WR_H1 + 11,
  WR_MIDNIGHT,              // optional MIDNIGHT tile: replaces "TWELVE" at/around 00:00
  WR_COUNT
};

struct WcfxLayoutWord { uint8_t role, x, y, len; };

enum WcfxGrammar : uint8_t {
  WCFX_GRAM_EXACT = 0,  // "IT IS TWENTY ONE MINUTES PAST SEVEN" (exact minute)
  WCFX_GRAM_FIVE  = 1,  // "IT IS TWENTY FIVE PAST SEVEN" (floored to 5-minute steps)
};

struct WcfxLayout {
  uint8_t width, height, grammar, wordCount;
  const WcfxLayoutWord *words;   // PROGMEM for the built-in tables, heap for custom
};

// ---- Stock layouts: layouts/*.json embedded at build, seeded to the FS at boot ---
// The JSON files in the repo's layouts/ folder are the single source of truth.
// layouts/gen_layouts.py (run automatically by PlatformIO via library.json's extraScript)
// embeds every layouts/*.json into layouts/_wcfx_layouts.generated.h as the WCFX_EMBEDDED[]
// array {path,json}; each is written to the filesystem if missing (delete a stock
// file to restore it on reboot). The embedded 16x16 doubles as the guaranteed
// fallback when the selected file is missing or invalid.
#include "layouts/_wcfx_layouts.generated.h"

// The embedded fallback layout: the 16x16 face if present, else the first entry.
static PGM_P wcfxFallbackJson() {
  for (unsigned i = 0; i < sizeof(WCFX_EMBEDDED)/sizeof(WCFX_EMBEDDED[0]); i++)
    if (strcmp(WCFX_EMBEDDED[i].path, WCFX_DEFAULT_LAYOUT_PATH) == 0) return WCFX_EMBEDDED[i].json;
  return WCFX_EMBEDDED[0].json;
}

// Ultimate fallback if even the flash JSON can't parse (OOM): renders background only.
static const WcfxLayout WCFX_LAYOUT_EMPTY = { 16, 16, WCFX_GRAM_EXACT, 0, nullptr };

// Fetch one word from a layout's word table (heap-resident — every layout is parsed
// from JSON into RAM).
static inline WcfxLayoutWord wcfxWordAt(const WcfxLayout &L, uint8_t i) {
  return L.words[i];
}

// Config/state mirrors maintained by the usermod, read by the (free) effect function.
static bool    wcfx_showPeriod = true;
static bool    wcfx_showTemp   = false;   // light a temperature word (if the layout has them)
static uint8_t wcfx_tempBand   = 0;       // 0 none, 1 COLD, 2 COOL, 3 WARM, 4 HOT
static const WcfxLayout *wcfx_layout    = &WCFX_LAYOUT_EMPTY;  // active layout
static uint8_t           wcfx_layoutGen = 0;  // bumped on layout change -> effect rebuilds

// OR one word's cells into the row bitmask (bit x of row y == cell lit).
static inline void wcSet(WcfxRow *mask, const WcfxLayout &L, const WcfxLayoutWord &w) {
  if (w.len == 0 || w.y >= L.height || w.y >= WCFX_MAX_H) return;
  for (uint8_t i = 0; i < w.len && (w.x + i) < L.width && (w.x + i) < WCFX_MAX_W; i++) {
    mask[w.y] |= (WcfxRow)1u << (w.x + i);
  }
}

// Light every table entry carrying this role (repeated roles = multi-segment words).
// Returns whether the layout has the role at all; missing roles are silent no-ops.
static bool wcfxLightRole(WcfxRow *mask, const WcfxLayout &L, uint8_t role) {
  bool hit = false;
  for (uint8_t i = 0; i < L.wordCount; i++) {
    const WcfxLayoutWord w = wcfxWordAt(L, i);
    if (w.role == role) { wcSet(mask, L, w); hit = true; }
  }
  return hit;
}

static inline void wcfxLightHour(WcfxRow *mask, const WcfxLayout &L, int h12) {
  if (h12 >= 1 && h12 <= 12) wcfxLightRole(mask, L, WR_H1 + (h12 - 1));
}

// Light the hour word, substituting MIDNIGHT for TWELVE — but only for the bare
// "IT IS MIDNIGHT" at exactly 00:00. Phrases with minutes must NOT use it: the 16x16
// MIDNIGHT tile sits above MINUTES/PAST/UNTIL on the face, so "... MINUTES UNTIL
// MIDNIGHT" renders out of reading order (bench-reported); those windows say TWELVE.
// Returns true only if MIDNIGHT was lit (so the caller can drop O'CLOCK and the
// period words — "IT IS MIDNIGHT" stands alone).
// Layouts without the tile fall back to the numeric hour (TWELVE), unchanged.
static inline bool wcfxLightHourOrMidnight(WcfxRow *mask, const WcfxLayout &L, int h12, bool isMidnight) {
  if (isMidnight && wcfxLightRole(mask, L, WR_MIDNIGHT)) return true;
  wcfxLightHour(mask, L, h12);
  return false;
}

// Exact-minute grammar: light the spoken minute value (1..30), "<N> MINUTES".
static void wcfxLightMinutesExact(WcfxRow *mask, const WcfxLayout &L, int val) {
  if (val == 15) { wcfxLightRole(mask, L, WR_A); wcfxLightRole(mask, L, WR_QUARTER); return; } // "A QUARTER", no MINUTES word
  if (val == 30) { wcfxLightRole(mask, L, WR_HALF); return; }                                  // no MINUTES word
  if (val <= 20) {
    wcfxLightRole(mask, L, WR_M1 + (val - 1));
  } else { // 21..29 -> TWENTY + ones
    wcfxLightRole(mask, L, WR_M20);
    wcfxLightRole(mask, L, WR_M1 + (val - 21));
  }
  wcfxLightRole(mask, L, WR_MINUTES);
}

// Five-minute grammar: light a value in {5,10,15,20,25,30}.
static void wcfxLightMinutesFive(WcfxRow *mask, const WcfxLayout &L, int val) {
  switch (val) {
    case  5: wcfxLightRole(mask, L, WR_M1 + 4); break;
    case 10: wcfxLightRole(mask, L, WR_M1 + 9); break;
    case 15: wcfxLightRole(mask, L, WR_A); wcfxLightRole(mask, L, WR_QUARTER); break;
    case 20: wcfxLightRole(mask, L, WR_M20); break;
    case 25: if (!wcfxLightRole(mask, L, WR_M25)) {      // dedicated TWENTYFIVE tile, else
               wcfxLightRole(mask, L, WR_M20);           // ... TWENTY + FIVE
               wcfxLightRole(mask, L, WR_M1 + 4);
             } break;
    case 30: wcfxLightRole(mask, L, WR_HALF); break;
  }
}

// Build the full active-letter bitmap from 24h hour and minute.
static void wcfxBuildMask(WcfxRow *mask, const WcfxLayout &L, int h24, int m) {
  for (int y = 0; y < WCFX_MAX_H; y++) mask[y] = 0;

  wcfxLightRole(mask, L, WR_IT);
  wcfxLightRole(mask, L, WR_IS);

  // Five-minute layouts floor to the last 5-minute step (10:04 still reads TEN O'CLOCK);
  // the corner-LED minute dots can show the remainder.
  const bool five = (L.grammar == WCFX_GRAM_FIVE);
  const int  mm   = five ? (m / 5) * 5 : m;

  int h12;
  bool litMidnight = false;
  if (mm == 0) {
    h12 = h24 % 12; if (h12 == 0) h12 = 12;
    litMidnight = wcfxLightHourOrMidnight(mask, L, h12, h24 == 0);
    if (!litMidnight) wcfxLightRole(mask, L, WR_OCLOCK);   // "IT IS MIDNIGHT", no O'CLOCK
  } else if (mm <= 30) {             // ... PAST <this hour>
    h12 = h24 % 12; if (h12 == 0) h12 = 12;
    if (five) wcfxLightMinutesFive(mask, L, mm);
    else      wcfxLightMinutesExact(mask, L, mm);
    wcfxLightRole(mask, L, WR_PAST);
    wcfxLightHour(mask, L, h12);     // ... PAST TWELVE at 00:01..00:30 (never MIDNIGHT: bad tile order)
  } else {                           // ... TO/UNTIL <next hour>
    int hn = (h24 + 1) % 24;
    h12 = hn % 12; if (h12 == 0) h12 = 12;
    if (five) wcfxLightMinutesFive(mask, L, 60 - mm);
    else      wcfxLightMinutesExact(mask, L, 60 - mm);
    wcfxLightRole(mask, L, WR_TO);
    wcfxLightHour(mask, L, h12);     // ... UNTIL TWELVE at 23:31..23:59 (never MIDNIGHT: bad tile order)
  }

  if (wcfx_showPeriod && !litMidnight) { // period of day based on real 24h hour; "IT IS MIDNIGHT" stands alone
    if (h24 < 12)      { wcfxLightRole(mask, L, WR_IN); wcfxLightRole(mask, L, WR_THE); wcfxLightRole(mask, L, WR_MORNING); }   // 00..11 (after midnight is morning)
    else if (h24 < 17) { wcfxLightRole(mask, L, WR_IN); wcfxLightRole(mask, L, WR_THE); wcfxLightRole(mask, L, WR_AFTERNOON); } // 12..16
    else if (h24 < 21) { wcfxLightRole(mask, L, WR_IN); wcfxLightRole(mask, L, WR_THE); wcfxLightRole(mask, L, WR_EVENING); }   // 17..20
    else               { wcfxLightRole(mask, L, WR_AT); wcfxLightRole(mask, L, WR_NIGHT); }                                     // 21..23
    wcfxLightRole(mask, L, (h24 < 12) ? WR_AM : WR_PM);  // layouts with AM/PM tiles instead
  }

  if (wcfx_showTemp && wcfx_tempBand >= 1 && wcfx_tempBand <= 4) {
    wcfxLightRole(mask, L, WR_AND);                          // '&' lights whenever a temperature word shows
    wcfxLightRole(mask, L, WR_COLD + (wcfx_tempBand - 1));   // COLD/COOL/WARM/HOT
  }
}

// Per-segment render state (kept in SEGENV.data so the effect is safe on 2+ segments).
struct WCFXRt {
  WcfxRow  cur[WCFX_MAX_H];
  WcfxRow  prev[WCFX_MAX_H];
  uint32_t transStart;
  uint8_t  lastBand;
  uint8_t  lastGen;
};

// ---- The effect ---------------------------------------------------------------
void mode_word_clock_fx(void) {
  // Requires a 2D matrix; fall back to solid color otherwise.
  if (!strip.isMatrix || !SEGMENT.is2D()) { SEGMENT.fill(SEGCOLOR(0)); return; }
  if (!SEGENV.allocateData(sizeof(WCFXRt))) { SEGMENT.fill(SEGCOLOR(0)); return; }
  WCFXRt &rt = *reinterpret_cast<WCFXRt *>(SEGENV.data); // zero-initialised on allocation

  const WcfxLayout &L = *wcfx_layout;
  const int cols = SEG_W;
  const int rows = SEG_H;

  const int h24 = hour(localTime);
  const int m   = minute(localTime);

  // Recompute the letter map only when the minute changes. On a change we keep the
  // previous map and crossfade to the new one over the segment's transition time, so
  // minute-to-minute changes fade in/out like a normal effect transition.
  const uint16_t stamp = (uint16_t)(h24 * 60 + m);
  if (SEGENV.call == 0 || SEGENV.aux0 != stamp || rt.lastBand != wcfx_tempBand || rt.lastGen != wcfx_layoutGen) {
    for (int y = 0; y < WCFX_MAX_H; y++) rt.prev[y] = (SEGENV.call == 0) ? 0 : rt.cur[y];
    wcfxBuildMask(rt.cur, L, h24, m);
    rt.transStart = strip.now;
    SEGENV.aux0 = stamp;
    rt.lastBand = wcfx_tempBand;
    rt.lastGen  = wcfx_layoutGen;
  }

  // Crossfade progress 0..255 driven by the segment/global transition setting.
  const uint16_t dur = strip.getTransition();
  uint8_t prog = 255;
  if (dur > 0) {
    const unsigned long el = strip.now - rt.transStart;
    prog = (el >= dur) ? 255 : (uint8_t)((el * 255) / dur);
  }

  const bool usePalette = SEGMENT.palette;
  const uint8_t bg = SEGMENT.intensity;            // "Background" dim level (0 = off)
  const int span = (cols * rows) > 0 ? (cols * rows) : 1;

  // The layout draws from the segment's top-left; frame it with the segment's 2D bounds.
  for (int y = 0; y < rows; y++) {
    const WcfxRow curRow  = (y < WCFX_MAX_H) ? rt.cur[y]  : 0;
    const WcfxRow prevRow = (y < WCFX_MAX_H) ? rt.prev[y] : 0;
    for (int x = 0; x < cols; x++) {
      const bool nowOn = (x < WCFX_MAX_W) && (curRow  & ((WcfxRow)1u << x));
      const bool wasOn = (x < WCFX_MAX_W) && (prevRow & ((WcfxRow)1u << x));
      uint32_t base = usePalette
        ? SEGMENT.color_from_palette((uint16_t)((x + y * cols) * 255 / span), true, false, 0)
        : SEGCOLOR(0);
      const uint32_t bgCol = bg ? color_fade(base, bg >> 2, true) : 0; // background ~1/4 brightness
      const uint32_t from = wasOn ? base : bgCol;
      const uint32_t to   = nowOn ? base : bgCol;
      SEGMENT.setPixelColorXY(x, y, (from == to) ? to : color_blend(from, to, prog));
    }
  }
}

// Effect metadata: name @ <speed(hidden)>,<intensity="Background"> ; color1 ; palette ; 2D ; default ix=0
static const char _data_FX_mode_word_clock_fx[] PROGMEM = "Word Clock FX@,Background;!;!;2;ix=0";

// Weather states (index into preset table / state names). 0 = unknown.
// ICE = freezing rain/drizzle; HAIL = thunderstorm w/ hail; HEAT/WIND derived from
// temperature / wind gusts (Oklahoma: heat waves, hail, ice storms, high wind).
enum WxState : uint8_t {
  WX_UNKNOWN=0, WX_CLEAR, WX_CLOUDS, WX_FOG, WX_DRIZZLE, WX_RAIN, WX_SNOW, WX_THUNDER,
  WX_ICE, WX_HAIL, WX_HEAT, WX_WIND, WX_SEVERE, WX_COUNT
};

// Config keys for the per-state presets, in WX_CLEAR..WX_SEVERE order (parallel to
// preset[WX_CLEAR + i]). One table drives both addToConfig and readFromConfig so the
// two directions can't drift. String literals live in flash on ESP32 (no RAM cost).
static const char* const WCFX_PRESET_KEYS[WX_COUNT - WX_CLEAR] = {
  "presetClear", "presetClouds", "presetFog", "presetDrizzle", "presetRain", "presetSnow",
  "presetThunder", "presetIce", "presetHail", "presetHeat", "presetWind", "presetSevere"
};

// ---- Usermod: registers the effect, resolves temperature, drives weather/presets --
class WordClockFxUsermod : public Usermod {
  private:
    bool enabled    = true;
    bool showPeriod = true;
    bool everConnected = false;   // first WiFi connect after boot has happened

    // Layout selection: a /wcfx-*.json filename in the FS root (stock faces are seeded).
    String          layoutFile   = WCFX_DEFAULT_LAYOUT_FILE;   // from the "default": true layout
    // Double-buffered layout storage. parseLayoutDoc() builds the new face into the
    // inactive slot, then atomically repoints wcfx_layout at it; the previously-active
    // slot's word table is freed only on the NEXT swap. This is what keeps the effect
    // safe: the render (loop task, core 1) binds `const WcfxLayout &L = *wcfx_layout`
    // and reads L.words for a whole frame while a settings save (async_tcp task, core 0)
    // runs parseLayoutDoc. An in-place free+overwrite would hand that live frame freed
    // memory / a torn struct; deferring the free to the next swap (frames are ~ms; layout
    // swaps are seconds apart, user-driven) guarantees no in-flight frame still reads it.
    WcfxLayout      layoutSlot[2] = { { 0,0,0,0,nullptr }, { 0,0,0,0,nullptr } };
    WcfxLayoutWord *slotWords[2]  = { nullptr, nullptr };  // heap table backing each slot
    uint8_t         activeSlot    = 0;
    String          layoutName;               // "name" from the loaded layout file
    String          layoutLink;               // "link" (docs URL) from the loaded layout file
    String          layoutStatus;             // parse result (shown on the Info page)
    uint8_t         wcfxEffectId = 255;       // effect id from strip.addEffect (gates minute dots)

    // Temperature words. All thresholds/values are in °C (band selection compares °C);
    // tempFahrenheit only changes the Info-page display unit.
    bool  showTemp      = false;
    bool  tempFahrenheit= false;
    float thrColdCool   = 10.0f;  // °C: below this -> COLD
    float thrCoolWarm   = 18.0f;  // °C: below this -> COOL
    float thrWarmHot    = 27.0f;  // °C: below this -> WARM, at/above -> HOT
    float manualTemp    = 20.0f;  // °C: fallback when no live value is available

    // Live temperature (from Open-Meteo or the JSON API); falls back to manualTemp once stale.
    static constexpr unsigned long LIVE_TTL = 30UL * 60UL * 1000UL; // 30 min
    unsigned long lastEval  = 0;
    float         curTemp   = 0.0f;       // last resolved value, °C (Info converts for display)
    float         liveTemp  = 0.0f;       // °C
    bool          liveValid = false;
    unsigned long liveTime  = 0;

    // Weather client (Open-Meteo, plain HTTP). This framework has TLS disabled, so HTTPS
    // sources (e.g. NWS) can't run on-device; push observed data via the JSON API instead.
    bool     fetchWeather    = false;
    bool     useWledLocation = true;
    bool     weatherPresets  = false;
    uint16_t fetchMinutes    = 15;
    String   place;                               // city or ZIP (used when not useWledLocation and set)
    float    latOverride     = 0.0f;
    float    lonOverride     = 0.0f;
    float    heatAbove       = 35.0f;             // °C: temp >= this -> HEAT (clear/cloudy)
    float    windAbove       = 60.0f;             // wind gust (km/h) >= this -> WIND (clear/cloudy)
    float    windGust        = 0.0f;
    bool     haveWind        = false;
    uint8_t  preset[WX_COUNT] = {};               // preset id per state (0 = none)
    uint8_t  wxState         = WX_UNKNOWN;
    uint8_t  lastApplied     = WX_UNKNOWN;
    bool     firstFetchDone  = false;
    bool     forceFetch      = false;             // "Update now" request
    bool     fetchSoon       = false;             // scheduled shortly after a (re)connect
    unsigned long fetchSoonAt= 0;
    uint8_t  pendingTest     = 0;                 // force-apply this state's preset (test)
    bool     lastTryFailed   = false;             // last fetch failed -> retry sooner
    unsigned long lastFetch  = 0;                 // last attempt
    unsigned long lastOkMs   = 0;                 // last successful weather parse
    bool     everOk          = false;
    static constexpr unsigned long RETRY_MS = 60UL * 1000UL; // retry 1 min after a failure
    float    humidity        = 0.0f;              // % relative humidity
    bool     haveHumidity    = false;
    // Geocoding cache for place -> coordinates.
    float    geoLat = 0.0f, geoLon = 0.0f;
    bool     geoDone = false;
    bool     geoFailed = false;                   // place could not be resolved
    String   geoFor;                              // place string the geo* were resolved for
    unsigned long geoNextTry = 0;                 // earliest retry after a geocode failure
    static constexpr unsigned long GEO_RETRY_MS = 60UL * 60UL * 1000UL; // re-geocode a failed place at most hourly

    // Corner buttons: light a mapped LED while its (native WLED) button is held.
    bool     cornerLeds = false;
    String   cornerColorHex = "FFFFFF";           // RRGGBB or RRGGBBWW
    uint32_t cornerColor = 0xFFFFFFUL;            // parsed from cornerColorHex
    // Minute dots: corner LEDs count minute % 5 — the minutes a 5-minute layout can't say.
    // Only meaningful on 5-minute grammars (an exact-minute face already shows the minute),
    // so the display is gated to those layouts; default on since that's where it helps.
    bool     minuteDots = true;
    // Corner order: Top-Left, Top-Right, Bottom-Left, Bottom-Right (matches readme wiring).
    int8_t   cbBtn[4] = { 1, 2, 3, 4 };           // WLED button index per corner (-1 = off)
    int16_t  cbLed[4] = { 257, 259, 256, 258 };   // LED pixel index per corner (-1 = off)
    int      testLed  = -1;                        // settings "Test": light this pixel briefly
    unsigned long testLedUntil = 0;

    static const char _name[];
    static const char _enabled[];
    static const char _showPeriod[];
    static const char _layout[];
    static const char _minuteDots[];
    static const char _showTemp[];
    static const char _fahrenheit[];
    static const char _thrColdCool[];
    static const char _thrCoolWarm[];
    static const char _thrWarmHot[];
    static const char _manualTemp[];
    static const char _fetch[];
    static const char _useWled[];
    static const char _interval[];
    static const char _place[];
    static const char _lat[];
    static const char _lon[];
    static const char _presets[];
    static const char _heatAbove[];
    static const char _windAbove[];
    // Per-state preset keys are in WCFX_PRESET_KEYS[] (file scope), not individual statics.

    uint8_t bandFor(float t) const {
      if (!showTemp) return 0;
      if (t < thrColdCool) return 1; // COLD
      if (t < thrCoolWarm) return 2; // COOL
      if (t < thrWarmHot)  return 3; // WARM
      return 4;                      // HOT
    }

    // All temperatures are kept in °C internally (thresholds, manualTemp, liveTemp, curTemp);
    // the fahrenheit flag only affects the Info-page display.
    void setTempCelsius(float c) {
      liveTemp = c;
      liveValid = true; liveTime = millis();
    }

    bool wledLocSet() const { return latitude != 0.0f || longitude != 0.0f; }

    // Location precedence: WLED Time-settings coords (if enabled AND actually set) ->
    // geocoded Place -> manual lat/lon. The "is it set" check means a Place still works
    // even with "use WLED location" ticked when WLED's coords are 0,0.
    float useLat() const {
      if (useWledLocation && wledLocSet()) return latitude;
      if (place.length())                  return geoDone ? geoLat : 0.0f;
      return latOverride;
    }
    float useLon() const {
      if (useWledLocation && wledLocSet()) return longitude;
      if (place.length())                  return geoDone ? geoLon : 0.0f;
      return lonOverride;
    }

    static String urlEncode(const String &s) {
      String o; char b[4];
      for (unsigned i = 0; i < s.length(); i++) {
        char c = s[i];
        if (isalnum((unsigned char)c)) o += c;
        else { sprintf(b, "%%%02X", (unsigned char)c); o += b; }
      }
      return o;
    }

    // Resolve place (city or ZIP) -> lat/lon via the Open-Meteo geocoding API.
    // Open-Meteo only matches the bare name, so drop any ", State/Country" qualifier.
    void geocode() {
      geoFor = place; geoDone = false; geoFailed = false;
      String name = place;
      int comma = name.indexOf(',');
      if (comma >= 0) name = name.substring(0, comma);
      name.trim();
      if (!name.length()) { geoFailed = true; return; }
      WiFiClient client;
      HTTPClient http;
      String url = F("http://geocoding-api.open-meteo.com/v1/search?count=1&name=");
      url += urlEncode(name);
      if (!http.begin(client, url)) { geoFailed = true; return; }
      http.setTimeout(2000);                 // cap how long this blocks loop(); only runs when Place changes
      if (http.GET() == HTTP_CODE_OK) {
        String payload = http.getString();
        StaticJsonDocument<96> filter;
        filter["results"][0]["latitude"]  = true;
        filter["results"][0]["longitude"] = true;
        StaticJsonDocument<256> doc;
        if (!deserializeJson(doc, payload, DeserializationOption::Filter(filter))) {
          JsonVariant la = doc["results"][0]["latitude"];
          JsonVariant lo = doc["results"][0]["longitude"];
          if (!la.isNull() && !lo.isNull()) {
            geoLat = la.as<float>(); geoLon = lo.as<float>();
            geoDone = true;
          }
        }
      }
      http.end();
      if (!geoDone) { geoFailed = true; geoNextTry = millis() + GEO_RETRY_MS; }
    }

    static const char* stateName(uint8_t s) {
      switch (s) {
        case WX_CLEAR:   return "clear";
        case WX_CLOUDS:  return "clouds";
        case WX_FOG:     return "fog";
        case WX_DRIZZLE: return "drizzle";
        case WX_RAIN:    return "rain";
        case WX_SNOW:    return "snow";
        case WX_THUNDER: return "thunder";
        case WX_ICE:     return "ice";
        case WX_HAIL:    return "hail";
        case WX_HEAT:    return "heat";
        case WX_WIND:    return "wind";
        case WX_SEVERE:  return "severe";
        default:         return "--";
      }
    }

    // Map a WMO weather interpretation code to a weather state.
    // Map a WMO weather code -> state. Note WX_SEVERE is intentionally NOT produced here:
    // Open-Meteo has no tornado/warning code, so SEVERE is driven externally (JSON API /
    // Home Assistant alert push, e.g. {"WordClockFx":{"wxtest":12}}).
    static uint8_t codeToState(int c) {
      if (c <= 1)                       return WX_CLEAR;     // 0 clear, 1 mainly clear
      if (c == 2 || c == 3)             return WX_CLOUDS;
      if (c == 45 || c == 48)           return WX_FOG;
      if (c == 56 || c == 57 || c == 66 || c == 67) return WX_ICE; // freezing drizzle/rain
      if (c >= 51 && c <= 55)           return WX_DRIZZLE;
      if ((c >= 61 && c <= 65) || (c >= 80 && c <= 82)) return WX_RAIN;
      if ((c >= 71 && c <= 77) || c == 85 || c == 86)   return WX_SNOW;
      if (c == 96 || c == 99)           return WX_HAIL;     // thunderstorm with hail
      if (c >= 95)                      return WX_THUNDER;   // 95
      return WX_UNKNOWN;
    }

    // Final state: condition code, with HEAT/WIND derived on otherwise calm skies.
    uint8_t computeState(int code, float tempC, float gust) const {
      uint8_t s = codeToState(code);
      if (s == WX_CLEAR || s == WX_CLOUDS) {     // only override on calm-sky conditions
        if (tempC >= heatAbove)      s = WX_HEAT;   // heatAbove in °C
        else if (gust >= windAbove)  s = WX_WIND;   // windAbove in km/h
      }
      return s;
    }

    // "RRGGBB" or "RRGGBBWW" hex -> packed RGBW. A malformed value falls back to white
    // rather than parsing to 0 (black), which would make the corner LEDs / minute dots
    // look dead with no indication why.
    static uint32_t parseHexColor(const String &s) {
      const size_t len = s.length();
      char *end = nullptr;
      const uint32_t v = (uint32_t)strtoul(s.c_str(), &end, 16);
      if ((len != 6 && len != 8) || end != s.c_str() + len)
        return RGBW32(255, 255, 255, 0);   // not clean 6/8-digit hex -> white
      if (len == 8) return RGBW32((v >> 24) & 0xFF, (v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF);
      return RGBW32((v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF, 0);
    }

    // Map a role token from a layout file to a WcfxRole; -1 if unknown.
    // Fixed names are in WcfxRole enum order; "until" aliases "to"; mN/hN are numeric.
    static int roleFromToken(const char *t) {
      static const char* const names[] = {
        "it","is","a","quarter","half","past","to","oclock","minutes","am","pm",
        "in","the","at","morning","afternoon","evening","night",
        "and","cold","cool","warm","hot"
      };
      for (unsigned i = 0; i < sizeof(names)/sizeof(names[0]); i++)
        if (!strcmp(t, names[i])) return (int)i;
      if (!strcmp(t, "until"))    return WR_TO;
      if (!strcmp(t, "amp"))      return WR_AND;   // legacy token for the '&' tile
      if (!strcmp(t, "midnight")) return WR_MIDNIGHT;
      if ((t[0] == 'm' || t[0] == 'h') && t[1] >= '0' && t[1] <= '9') {
        const int n = atoi(t + 1);
        if (t[0] == 'm') {
          if (n >= 1 && n <= 20) return WR_M1 + (n - 1);
          if (n == 25)           return WR_M25;
        } else if (n >= 1 && n <= 12) return WR_H1 + (n - 1);
      }
      return -1;
    }

    // Sanitize a string for embedding in the settings-page JS: one weird layout file
    // must not be able to break the whole page (unbalanced quote = all fields render raw).
    static String wcfxJsSan(const String &s, uint8_t maxLen = 128) {
      String o; o.reserve(s.length());
      for (unsigned i = 0; i < s.length() && i < maxLen; i++) {
        const char c = s[i];
        o += (c == '"' || c == '\'' || c == '\\' || c == '<' || c == '>' || (uint8_t)c < 0x20) ? '_' : c;
      }
      return o;
    }
    // One JS array entry: ["file","name","link"], (trailing comma is legal in JS).
    static String wcfxJsEntry(const String &fn, const String &nm, const String &lk) {
      String out = F("[\"");
      out += wcfxJsSan(fn);     out += F("\",\"");
      out += wcfxJsSan(nm, 40); out += F("\",\"");
      out += wcfxJsSan(lk);     out += F("\"],");
      return out;
    }

    // Shared back half of the layout loaders: validate the parsed JSON and swap it in.
    // On success the heap word table replaces the previous one (wcfx_layout is repointed
    // to a safe layout BEFORE the old table is freed — never dangling; settings saves run
    // in the async_tcp task while the effect renders from loop). On failure the currently
    // active layout is left untouched and layoutStatus carries the error.
    bool parseLayoutDoc(JsonDocument &doc, const char *src) {
      const int w = doc["width"]  | (doc["w"] | 0);   // short keys accepted for compat
      const int h = doc["height"] | (doc["h"] | 0);
      const char *gs = doc["grammar"] | "five";
      uint8_t grammar;
      if      (strcmp(gs, "five")  == 0) grammar = WCFX_GRAM_FIVE;
      else if (strcmp(gs, "exact") == 0) grammar = WCFX_GRAM_EXACT;
      else { layoutStatus = F("error: grammar must be 'five' or 'exact'"); return false; }
      if (w < 1 || w > WCFX_MAX_W || h < 1 || h > WCFX_MAX_H) {
        layoutStatus = String(F("error: width/height out of range (1..")) + WCFX_MAX_W + ')';
        return false;
      }
      JsonArray words = doc["words"];
      if (words.isNull() || words.size() < 1 || words.size() > 96) {
        layoutStatus = F("error: 'words' must be an array of 1..96 entries");
        return false;
      }

      WcfxLayoutWord *tbl = new (std::nothrow) WcfxLayoutWord[words.size()];
      if (!tbl) { layoutStatus = F("error: out of memory"); return false; }
      uint8_t n = 0;
      for (JsonVariant v : words) {
        JsonArray e = v.as<JsonArray>();
        const char *tok = e[0] | (const char*)nullptr;
        const int role = tok ? roleFromToken(tok) : -1;
        const int x = e[1] | -1, y = e[2] | -1, len = e[3] | -1;
        if (role < 0 || x < 0 || y < 0 || len < 1 || x + len > w || y >= h) {
          layoutStatus = String(F("error: word ")) + n + F(" ('") + (tok ? tok : "?") + F("') invalid");
          delete[] tbl;
          return false;
        }
        tbl[n++] = { (uint8_t)role, (uint8_t)x, (uint8_t)y, (uint8_t)len };
      }
      layoutName = doc["name"] | src;    // display name; falls back to the source label
      layoutLink = doc["link"] | "";     // docs URL for the Info page / settings link
      // Both render into the Info page via innerHTML — strip HTML-significant chars so a
      // crafted layout file can't inject markup/script, and cap the name so it can't
      // overflow the Info panel (settings-page side is sanitized separately by wcfxJsSan).
      layoutName.replace("\"", ""); layoutName.replace("<", ""); layoutName.replace(">", "");
      if (layoutName.length() > 40) layoutName = layoutName.substring(0, 40);
      layoutLink.replace("\"", ""); layoutLink.replace("<", "");  // injection-safe

      // Publish the new face into the inactive slot, then repoint. The word table that
      // backs the slot we're about to overwrite belonged to the buffer-before-last, so no
      // effect frame still references it — freeing it here is the deferred free (see the
      // layoutSlot[] declaration). The active slot / wcfx_layout stay valid until the
      // single-pointer publish below, so a concurrent render never sees a half-built face.
      const uint8_t next = activeSlot ^ 1;
      delete[] slotWords[next];
      slotWords[next]  = tbl;
      layoutSlot[next] = { (uint8_t)w, (uint8_t)h, grammar, n, tbl };
      wcfx_layout = &layoutSlot[next];
      activeSlot  = next;
      layoutStatus = String(F("ok: ")) + n + F(" words, ") + w + 'x' + h + ' ' + gs;
      return true;
    }

    // Only the fields the firmware needs enter the parse pool — extra/documentation
    // fields like "letters" (used by the settings-page grid preview, fetched client-side)
    // are filtered out so they never cost device RAM.
    static void fillLayoutFilter(JsonDocument &f) {
      f["name"] = true; f["link"] = true; f["grammar"] = true; f["words"] = true;
      f["width"] = true; f["height"] = true; f["w"] = true; f["h"] = true;
    }

    // Load a layout from a filesystem file (uploaded via WLED's /edit page).
    // The doc is our own arena, never WLED's pinned doc; 8k — the 16x16 file needs >4k.
    bool loadLayoutFile(const char *path) {
      File f = WLED_FS.open(path, "r");
      if (!f) { layoutStatus = String(F("error: ")) + path + F(" not found"); return false; }
      StaticJsonDocument<192> filter; fillLayoutFilter(filter);
      DynamicJsonDocument doc(8192);
      const DeserializationError err = deserializeJson(doc, f, DeserializationOption::Filter(filter));
      f.close();
      if (err) { layoutStatus = String(F("error: ")) + err.c_str(); return false; }
      return parseLayoutDoc(doc, path);
    }

    // Load one of the embedded stock layouts straight from flash.
    bool loadLayoutFlash(PGM_P json, const char *label) {
      StaticJsonDocument<192> filter; fillLayoutFilter(filter);
      DynamicJsonDocument doc(8192);
      const DeserializationError err = deserializeJson(doc, FPSTR(json), DeserializationOption::Filter(filter));
      if (err) { layoutStatus = String(F("error: ")) + err.c_str(); return false; }
      return parseLayoutDoc(doc, label);
    }

    // Seed the stock layout files if missing (user edits are preserved; delete a
    // stock file — or send {"WordClockFx":{"reseedLayouts":true}} — to restore stock).
    static void seedLayoutFile(const char *path, PGM_P json, bool force = false) {
      if (!force && WLED_FS.exists(path)) return;
      File f = WLED_FS.open(path, "w");
      if (f) { f.print(FPSTR(json)); f.close(); }
    }
    // Force-rewrite every stock file from the embedded copies (updates propagate;
    // in-place edits to STOCK files are replaced — user-named layouts are untouched).
    static void reseedLayoutFiles() {
      for (unsigned i = 0; i < sizeof(WCFX_EMBEDDED)/sizeof(WCFX_EMBEDDED[0]); i++)
        seedLayoutFile(WCFX_EMBEDDED[i].path, WCFX_EMBEDDED[i].json, true);
    }
    static void seedLayoutFiles() {
      // Pre-v1.6.0 file migrations (before seeding, so one firmware-managed copy remains):
      // the old default /wcfx-16x16.json IS the MK1 face — carry user edits to its new name.
      if (WLED_FS.exists(F("/wcfx-16x16.json")) && !WLED_FS.exists(F("/wcfx-16x16-mk1.json")))
        WLED_FS.rename(F("/wcfx-16x16.json"), F("/wcfx-16x16-mk1.json"));
      // v1.3.0 kept the custom layout in /wordclock.json; move it into the wcfx- scheme.
      if (WLED_FS.exists(F("/wordclock.json")) && !WLED_FS.exists(F("/wcfx-custom.json")))
        WLED_FS.rename(F("/wordclock.json"), F("/wcfx-custom.json"));
      for (unsigned i = 0; i < sizeof(WCFX_EMBEDDED)/sizeof(WCFX_EMBEDDED[0]); i++)
        seedLayoutFile(WCFX_EMBEDDED[i].path, WCFX_EMBEDDED[i].json);
    }

    // Load the selected layout file, falling back to the embedded default face when
    // it's missing/invalid. Called from setup(), readFromConfig() and the reloadLayout API.
    void applyLayout() {
      const String path = layoutFile.startsWith("/") ? layoutFile : String('/') + layoutFile;
      if (!loadLayoutFile(path.c_str())) {
        const String err = layoutStatus;   // keep the real error for the Info page
        if (!loadLayoutFlash(wcfxFallbackJson(), "built-in fallback")) wcfx_layout = &WCFX_LAYOUT_EMPTY;
        layoutStatus = err + F(" (using built-in fallback)");
      }
      wcfx_layoutGen++;                    // effect rebuilds (crossfades) on next frame
    }

    // True when any active segment is running the Word Clock FX effect.
    bool effectActive() const {
      for (unsigned i = 0; i < strip.getSegmentsNum(); i++) {
        const Segment &s = strip.getSegment(i);
        if (s.isActive() && s.mode == wcfxEffectId) return true;
      }
      return false;
    }

    void applyStatePreset() {
      if (!weatherPresets || wxState == WX_UNKNOWN || wxState == lastApplied) return;
      const uint8_t p = preset[wxState];
      lastApplied = wxState;                 // remember even if 0, so we only act on changes
      if (p > 0) applyPreset(p, CALL_MODE_DIRECT_CHANGE);
    }

    bool fetch() {
      const bool needPlace = !(useWledLocation && wledLocSet()) && place.length();
      // (Re)geocode only when the place changed, when it was never attempted, or when a
      // prior failure's hourly backoff has elapsed. Without the backoff a typo'd Place
      // would re-hit the geocoding API every RETRY_MS (60s) forever, since a failure
      // leaves geoDone=false and the weather retry loop calls fetch() every minute.
      if (needPlace) {
        const bool placeChanged = (geoFor != place);
        if (placeChanged || (!geoDone && (!geoFailed || (long)(millis() - geoNextTry) >= 0)))
          geocode();
      }
      const float la = useLat(), lo = useLon();
      if (la == 0.0f && lo == 0.0f) return false; // location not set/unresolved
      bool ok = fetchOpenMeteo(la, lo);
      if (ok) { lastOkMs = millis(); everOk = true; }
      return ok;
    }

    bool fetchOpenMeteo(float la, float lo) {
      WiFiClient client;                     // plain HTTP (Open-Meteo serves the API on :80)
      HTTPClient http;
      char url[224];
      snprintf(url, sizeof(url),
        "http://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
        "&current=temperature_2m,relative_humidity_2m,weather_code,wind_gusts_10m",
        la, lo);
      if (!http.begin(client, url)) return false;
      http.setTimeout(2000);                 // cap how long this blocks loop(); fetches are infrequent + retried
      bool ok = false;
      if (http.GET() == HTTP_CODE_OK) {
        String payload = http.getString();
        StaticJsonDocument<192> filter;
        filter["current"]["temperature_2m"]      = true;
        filter["current"]["relative_humidity_2m"]= true;
        filter["current"]["weather_code"]        = true;
        filter["current"]["wind_gusts_10m"]      = true;
        StaticJsonDocument<320> doc;
        if (!deserializeJson(doc, payload, DeserializationOption::Filter(filter))) {
          JsonObject cur = doc["current"];
          haveHumidity = false; haveWind = false; // clear; only set true if THIS response carries them
          float tC = NAN;
          JsonVariant t = cur["temperature_2m"];
          if (!t.isNull()) { tC = t.as<float>(); setTempCelsius(tC); ok = true; }
          JsonVariant h = cur["relative_humidity_2m"];
          if (!h.isNull()) { humidity = h.as<float>(); haveHumidity = true; }
          JsonVariant g = cur["wind_gusts_10m"];
          if (!g.isNull()) { windGust = g.as<float>(); haveWind = true; }
          JsonVariant w = cur["weather_code"];
          if (!w.isNull()) {
            const float tForBands = isnan(tC) ? curTemp : tC; // °C
            wxState = computeState(w.as<int>(), tForBands, haveWind ? windGust : 0.0f);
            applyStatePreset();
            ok = true;
          }
        }
      }
      http.end();
      return ok;
    }


  public:
    void setup() override {
      if (enabled) {
        wcfxEffectId = strip.addEffect(255, &mode_word_clock_fx, _data_FX_mode_word_clock_fx);
      }
      wcfx_showPeriod = showPeriod;
      wcfx_showTemp   = showTemp;
      // readFromConfig already ran (pre-seed it falls back to the embedded 16x16);
      // seed the stock files, then load the selected one for real.
      seedLayoutFiles();
      applyLayout();
    #ifdef WCFX_DEFAULT_TRANSITION_MS
      // Override the boot transition (runs after cfg load), set via build flag in the
      // platformio override, e.g. -D WCFX_DEFAULT_TRANSITION_MS=1800 for 1.8 s.
      transitionDelay = transitionDelayDefault = WCFX_DEFAULT_TRANSITION_MS;
      strip.setTransition(transitionDelay);
    #endif
    }

    // Called whenever WiFi (re)connects (like NTP): fetch weather shortly after. On the
    // FIRST connect after boot, also reset lastApplied so the current weather's preset is
    // applied once; later reconnects refresh data but won't override a manual selection.
    void connected() override {
      if (!enabled) return;
      fetchSoon   = true;
      fetchSoonAt = millis() + 3000; // let the network stack/NTP settle first
      if (!everConnected) { lastApplied = WX_UNKNOWN; everConnected = true; }
    }

    void loop() override {
      if (!enabled) return;
      const unsigned long now = millis();

      // Resolve temperature -> band at most once per second.
      if (now - lastEval >= 1000) {
        lastEval = now;
        const bool fresh = liveValid && (now - liveTime) < LIVE_TTL;
        curTemp = fresh ? liveTemp : manualTemp;
        if (liveValid && !fresh) wxState = WX_UNKNOWN; // stale: don't present old condition as current
        wcfx_tempBand = bandFor(curTemp);
      }

      // Force-apply a weather state's preset for testing (bypasses the change check).
      if (pendingTest) {
        const uint8_t s = pendingTest; pendingTest = 0;
        wxState = s; lastApplied = s; everOk = true;
        if (preset[s] > 0) applyPreset(preset[s], CALL_MODE_DIRECT_CHANGE);
      }

      if (!WLED_CONNECTED) return;

      // "Update now" request (from the settings button / JSON API).
      if (forceFetch) { forceFetch = false; lastTryFailed = !fetch(); lastFetch = now; firstFetchDone = true; return; }

      if (!fetchWeather) return;

      if (fetchSoon) {                              // shortly after a (re)connect
        if ((long)(now - fetchSoonAt) >= 0) { fetchSoon = false; lastTryFailed = !fetch(); lastFetch = now; firstFetchDone = true; }
      } else if (!firstFetchDone) {                 // fallback if connected() never fired
        if (now >= 30000) { lastTryFailed = !fetch(); lastFetch = now; firstFetchDone = true; }
      } else {                                      // periodic; retry sooner after a failure
        const unsigned long due = lastTryFailed ? RETRY_MS : (unsigned long)fetchMinutes * 60000UL;
        if (now - lastFetch >= due) { lastTryFailed = !fetch(); lastFetch = now; }
      }
    }

    // Light the mapped corner LED while its native WLED button is held. Runs after all
    // effects, just before show(), so it overrides the corner segment's normal output.
    void handleOverlayDraw() override {
      const uint16_t total = strip.getLengthTotal();
      // Minute dots: corners count minute % 5 (the remainder a 5-minute layout can't
      // say), filling in cbLed[] order. Drawn before the button feedback so a held
      // button still overrides its corner. Gated to 5-minute grammars — an exact-minute
      // face already spells out the minute, so dots there would be redundant.
      if (minuteDots && enabled && wcfx_layout->grammar == WCFX_GRAM_FIVE && effectActive()) {
        const int dots = minute(localTime) % 5;
        for (int i = 0; i < dots && i < 4; i++) {
          if (cbLed[i] < 0 || cbLed[i] >= (int)total) continue;
          strip.setPixelColor((uint16_t)cbLed[i], cornerColor);
        }
      }
      if (cornerLeds) {
        for (int i = 0; i < 4; i++) {
          if (cbBtn[i] < 0 || cbLed[i] < 0 || cbLed[i] >= (int)total) continue;
          if (cbBtn[i] < WLED_MAX_BUTTONS && isButtonPressed((uint8_t)cbBtn[i]))
            strip.setPixelColor((uint16_t)cbLed[i], cornerColor);
        }
      }
      // settings "Test" button: light a pixel for a few seconds (works even if disabled).
      if (testLed >= 0 && testLed < (int)total && millis() < testLedUntil)
        strip.setPixelColor((uint16_t)testLed, cornerColor);
    }

    // JSON API: {"WordClockFx":{"temp":22.5}} pushes a temperature in °C;
    //           {"WordClockFx":{"update":true}} fetches weather now.
    void readFromJsonState(JsonObject &root) override {
      JsonObject top = root[FPSTR(_name)];
      if (top.isNull()) return;
      float t;
      if (getJsonValue(top[F("temp")], t)) setTempCelsius(t);
      if (top[F("update")].as<bool>()) forceFetch = true;
      int n;
      if (getJsonValue(top[F("wxtest")], n) && n >= 1 && n < WX_COUNT) pendingTest = (uint8_t)n;
      if (getJsonValue(top[F("ledtest")], n) && n >= 0) { testLed = n; testLedUntil = millis() + 3000; }
      if (top[F("reloadLayout")].as<bool>()) applyLayout();   // re-read the layout file without reboot
      if (top[F("reseedLayouts")].as<bool>()) { reseedLayoutFiles(); applyLayout(); }  // restore stock files
    }

    void addToJsonInfo(JsonObject &root) override {
      JsonObject user = root[F("u")];
      if (user.isNull()) user = root.createNestedObject(F("u"));
      user.createNestedArray(F("Word Clock FX")).add(F("v" WCFX_VERSION));

      // Layout row: name as a clickable docs link (Info values render via innerHTML,
      // same mechanism as the weather source link); parse errors show as plain text.
      JsonArray aLay = user.createNestedArray(F("Word Clock layout"));
      if (layoutStatus.startsWith(F("error"))) {
        aLay.add(layoutStatus);
      } else {
        String v = layoutName.length() ? layoutName : layoutStatus;
        if (layoutLink.length())
          v = String(F("<a href=\"")) + layoutLink + F("\" target=\"_blank\">") + v + F("</a>");
        aLay.add(v);
      }

      const bool wx = fetchWeather || everOk;   // also show once a manual fetch has succeeded
      if (!showTemp && !wx) return;
      char buf[24];

      const float shownTemp = tempFahrenheit ? (curTemp * 9.0f / 5.0f + 32.0f) : curTemp;
      snprintf(buf, sizeof(buf), "%.1f", shownTemp);
      JsonArray aT = user.createNestedArray(F("Word Clock temperature"));
      aT.add(buf); aT.add(tempFahrenheit ? F(" °F") : F(" °C"));

      if (wx) {
        if (haveHumidity) {
          snprintf(buf, sizeof(buf), "%.0f", humidity);
          JsonArray aH = user.createNestedArray(F("Word Clock humidity"));
          aH.add(buf); aH.add(F(" %"));
        }
        // Surface the live wind gust so "Wind gust above" can be tuned against a real
        // reading (it drives the WIND weather state but was otherwise invisible).
        if (haveWind) {
          snprintf(buf, sizeof(buf), "%.0f", windGust);
          JsonArray aW = user.createNestedArray(F("Word Clock wind gust"));
          aW.add(buf); aW.add(F(" km/h"));
        }
        const bool fresh = liveValid && (millis() - liveTime) < LIVE_TTL;
        JsonArray aC = user.createNestedArray(F("Word Clock condition"));
        aC.add((everOk && !fresh) ? "stale" : stateName(wxState));

        char loc[72];
        locationInfo(loc, sizeof(loc));
        JsonArray aL = user.createNestedArray(F("Word Clock location"));
        aL.add(loc);

        // Exact Open-Meteo query used (handy for debugging). Render as a short clickable link
        // ("open-meteo.com") so the long URL doesn't overflow the Info panel; the full URL is the
        // href. The settings status panel parses the href back out for its "view source" link.
        const float la = useLat(), lo = useLon();
        if (la != 0.0f || lo != 0.0f) {
          char link[256];
          snprintf(link, sizeof(link),
            "<a href=\"https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
            "&current=temperature_2m,relative_humidity_2m,weather_code,wind_gusts_10m\""
            " target=\"_blank\">open-meteo.com</a>",
            la, lo);
          user.createNestedArray(F("Word Clock source")).add(link);
        }

        JsonArray aU = user.createNestedArray(F("Word Clock updated"));
        if (!everOk) aU.add(F("never"));
        else {
          const unsigned long s = (millis() - lastOkMs) / 1000UL;
          if (s < 90)        { snprintf(buf, sizeof(buf), "%lus ago", s); aU.add(buf); }
          else               { snprintf(buf, sizeof(buf), "%lum ago", s / 60UL); aU.add(buf); }
        }
      }
    }

    void locationInfo(char *buf, size_t n) {
      if (useWledLocation && wledLocSet()) {
        snprintf(buf, n, "%.4f, %.4f (WLED)", latitude, longitude);
      } else if (place.length()) {
        if (geoDone)        snprintf(buf, n, "%s (%.4f, %.4f)", place.c_str(), geoLat, geoLon);
        else if (geoFailed) snprintf(buf, n, "'%s' not found", place.c_str());
        else                snprintf(buf, n, "geocoding...");
      } else if (latOverride != 0.0f || lonOverride != 0.0f) {
        snprintf(buf, n, "%.4f, %.4f", latOverride, lonOverride);
      } else {
        snprintf(buf, n, "location unset");
      }
    }

    void addToConfig(JsonObject &root) override {
      JsonObject top = root.createNestedObject(FPSTR(_name));
      // Order here = settings render order: Display, Corner buttons, Weather, Location,
      // Weather->Presets, Temperature words.
      top[FPSTR(_enabled)]     = enabled;
      top[FPSTR(_showPeriod)]  = showPeriod;
      top[FPSTR(_layout)]      = layoutFile;   // String -> form "text" descriptor, round-trips
      top[F("cornerLeds")]     = cornerLeds;
      top[F("cornerColor")]    = cornerColorHex;
      top[FPSTR(_minuteDots)]  = minuteDots;
      char ck[8];
      for (uint8_t i = 0; i < 4; i++) {
        snprintf(ck, sizeof(ck), "cbBtn%u", (unsigned)i); top[ck] = cbBtn[i];
        snprintf(ck, sizeof(ck), "cbLed%u", (unsigned)i); top[ck] = cbLed[i];
      }
      top[FPSTR(_fetch)]       = fetchWeather;
      top[FPSTR(_interval)]    = fetchMinutes;
      top[FPSTR(_useWled)]     = useWledLocation;
      top[FPSTR(_place)]       = place;
      top[FPSTR(_lat)]         = latOverride;
      top[FPSTR(_lon)]         = lonOverride;
      top[FPSTR(_presets)]     = weatherPresets;
      top[FPSTR(_heatAbove)]   = heatAbove;
      top[FPSTR(_windAbove)]   = windAbove;
      for (uint8_t i = 0; i < WX_COUNT - WX_CLEAR; i++)
        top[WCFX_PRESET_KEYS[i]] = preset[WX_CLEAR + i];
      top[FPSTR(_showTemp)]    = showTemp;
      top[FPSTR(_fahrenheit)]  = tempFahrenheit;
      top[FPSTR(_thrColdCool)] = thrColdCool;
      top[FPSTR(_thrCoolWarm)] = thrCoolWarm;
      top[FPSTR(_thrWarmHot)]  = thrWarmHot;
      top[FPSTR(_manualTemp)]  = manualTemp;
    }

    bool readFromConfig(JsonObject &root) override {
      JsonObject top = root[FPSTR(_name)];
      bool configComplete = !top.isNull();
      configComplete &= getJsonValue(top[FPSTR(_enabled)],     enabled);
      configComplete &= getJsonValue(top[FPSTR(_showPeriod)],  showPeriod);
      // Layout = filename. v1.3.0 stored an int 0/1/2 — migrate (ArduinoJson stringifies
      // stored ints, so the old 1 reads back as "1"); persisted on the next settings save.
      String ls;
      configComplete &= getJsonValue(top[FPSTR(_layout)],      ls, WCFX_DEFAULT_LAYOUT_FILE);
      if      (ls == F("0")) ls = F(WCFX_DEFAULT_LAYOUT_FILE);
      else if (ls == F("1")) ls = F("wcfx-11x10.json");
      else if (ls == F("2")) ls = F("wcfx-custom.json");           // was /wordclock.json (renamed at boot)
      else if (ls == F("wcfx-16x16.json")) ls = F(WCFX_DEFAULT_LAYOUT_FILE);  // pre-MK3 default
      if (ls.length()) layoutFile = ls;
      configComplete &= getJsonValue(top[FPSTR(_showTemp)],    showTemp);
      configComplete &= getJsonValue(top[FPSTR(_fahrenheit)],  tempFahrenheit);
      configComplete &= getJsonValue(top[FPSTR(_thrColdCool)], thrColdCool);
      configComplete &= getJsonValue(top[FPSTR(_thrCoolWarm)], thrCoolWarm);
      configComplete &= getJsonValue(top[FPSTR(_thrWarmHot)],  thrWarmHot);
      configComplete &= getJsonValue(top[FPSTR(_manualTemp)],  manualTemp);
      configComplete &= getJsonValue(top[FPSTR(_fetch)],       fetchWeather);
      configComplete &= getJsonValue(top[FPSTR(_useWled)],     useWledLocation);
      configComplete &= getJsonValue(top[FPSTR(_interval)],    fetchMinutes);
      configComplete &= getJsonValue(top[FPSTR(_place)],       place);
      configComplete &= getJsonValue(top[FPSTR(_lat)],         latOverride);
      configComplete &= getJsonValue(top[FPSTR(_lon)],         lonOverride);
      if (geoFor != place) geoDone = false; // place changed -> re-geocode on next fetch
      configComplete &= getJsonValue(top[FPSTR(_presets)],     weatherPresets);
      configComplete &= getJsonValue(top[FPSTR(_heatAbove)],   heatAbove);
      configComplete &= getJsonValue(top[FPSTR(_windAbove)],   windAbove);
      for (uint8_t i = 0; i < WX_COUNT - WX_CLEAR; i++)
        configComplete &= getJsonValue(top[WCFX_PRESET_KEYS[i]], preset[WX_CLEAR + i]);
      configComplete &= getJsonValue(top[F("cornerLeds")],     cornerLeds);
      configComplete &= getJsonValue(top[F("cornerColor")],    cornerColorHex);
      configComplete &= getJsonValue(top[FPSTR(_minuteDots)],  minuteDots, true);
      char ck[8];
      for (uint8_t i = 0; i < 4; i++) {
        snprintf(ck, sizeof(ck), "cbBtn%u", (unsigned)i); configComplete &= getJsonValue(top[ck], cbBtn[i]);
        snprintf(ck, sizeof(ck), "cbLed%u", (unsigned)i); configComplete &= getJsonValue(top[ck], cbLed[i]);
      }
      cornerColor = parseHexColor(cornerColorHex);
      if (fetchMinutes < 1) fetchMinutes = 1;
      // Keep the temperature bands monotonic (cold <= cool <= warm) so a bad config
      // can't leave a band unreachable. Raise each cut to the one below it.
      if (thrCoolWarm < thrColdCool) thrCoolWarm = thrColdCool;
      if (thrWarmHot  < thrCoolWarm) thrWarmHot  = thrCoolWarm;
      wcfx_showPeriod = showPeriod;
      wcfx_showTemp   = showTemp;
      applyLayout();
      return configComplete;
    }

    void appendConfigData() {
      // ---- styling ------------------------------------------------------------
      oappend(F("(function(){var s=document.createElement('style');s.innerHTML="
                "'.wcfxh{margin:18px 14px 6px;padding-bottom:2px;font-weight:600;color:#4aa3ff;border-bottom:1px solid #2c2c2c;letter-spacing:.3px}'"
                "+'#wcfxstat,.wcfxcard{background:#101010;border:1px solid #2c2c2c;border-radius:8px;padding:8px 10px;margin:4px 14px;display:block;line-height:1.6}'"
                "+'#um button{cursor:pointer;border-radius:6px;padding:2px 9px}'"
                "+'.wcfxtbl{border-collapse:collapse;margin:4px 14px}'"
                "+'.wcfxtbl th,.wcfxtbl td{padding:3px 12px 3px 0;text-align:left;vertical-align:middle}'"
                "+'.wcfxtbl th{color:#4aa3ff;font-weight:600;border-bottom:1px solid #2c2c2c}'"
                "+'.wcfxtbl select,.wcfxtbl input:not([type=checkbox]){margin:0;vertical-align:middle;height:26px;box-sizing:border-box}'"
                "+'.wcfxtbl input:not([type=checkbox]){width:74px}'"
                "+'.wcfxtbl input[type=checkbox]{margin:0;vertical-align:middle}'"
                "+'.wcfxtbl button{margin:0;vertical-align:middle}'"
                "+'.wcfxi{font-size:11px;opacity:.6;font-style:normal;margin-left:4px}'"
                "+'.wcfxlnk{margin-left:8px;font-size:13px;color:#4aa3ff;text-decoration:none}'"
                "+'.wcfxlnk:hover{text-decoration:underline}'"
                "+'#wcfxgrid{font-family:monospace;letter-spacing:.8em;line-height:1.45;display:inline-block;white-space:pre}'"
                "+'#wcfxgrid .d{opacity:.3}'"
                ";document.head.appendChild(s);})();"));

      // ---- helpers: section header, relabel, and move fields into a table ------
      oappend(F("wcfxsec=function(fld,t){var a=d.getElementsByName('WordClockFx:'+fld);if(!a.length)return;"
                "var f=a[0],r=f.previousSibling;var h=document.createElement('div');h.className='wcfxh';h.textContent=t;"
                "f.parentNode.insertBefore(h,(r&&r.nodeType===3)?r:f);};"));
      oappend(F("wcfxlbl=function(fld,t){var a=d.getElementsByName('WordClockFx:'+fld);if(!a.length)return;"
                "var r=a[0].previousSibling;if(r&&r.nodeType===3)r.textContent=' '+t+' ';};"));
      // wcfxtbl(headers, rows): rows = [label, [fieldNames...], extra(tr,cells)|null]. Moves the
      // existing inputs into a table; cleans up stray label text/<br>; inserts at first field.
      oappend(F("wcfxtbl=function(hdr,rows){var ok=0;for(var r=0;r<rows.length;r++)if(d.getElementsByName('WordClockFx:'+rows[r][1][0]).length){ok=1;break;}if(!ok)return;"
                "var t=document.createElement('table');t.className='wcfxtbl';var h=document.createElement('tr');"
                "for(var i=0;i<hdr.length;i++){var th=document.createElement('th');th.textContent=hdr[i];h.appendChild(th);}t.appendChild(h);"
                "var anchor=null,kill=[];function mv(td,nm){var e=d.getElementsByName('WordClockFx:'+nm);if(!e.length)return;"
                "var lbl=e[0].previousSibling,af=e[e.length-1].nextSibling;if(!anchor)anchor=(lbl&&lbl.nodeType===3)?lbl:e[0];"
                "var a=[];for(var k=0;k<e.length;k++)a.push(e[k]);for(k=0;k<a.length;k++)td.appendChild(a[k]);"
                "if(lbl&&lbl.nodeType===3)kill.push(lbl);if(af&&af.nodeName==='BR')kill.push(af);}"
                "for(var ri=0;ri<rows.length;ri++){var row=rows[ri];var tr=document.createElement('tr');"
                "var c0=document.createElement('td');c0.textContent=row[0];tr.appendChild(c0);var cells=[];"
                "for(var fi=0;fi<row[1].length;fi++){var td=document.createElement('td');mv(td,row[1][fi]);tr.appendChild(td);cells.push(td);}"
                "if(row[2])row[2](tr,cells);t.appendChild(tr);}"
                "if(anchor&&anchor.parentNode)anchor.parentNode.insertBefore(t,anchor);"
                "for(ri=0;ri<kill.length;ri++)if(kill[ri].parentNode)kill[ri].parentNode.removeChild(kill[ri]);};"));
      // test-cell builders for the tables
      oappend(F("wcfxwxTest=function(s){return function(tr){var td=document.createElement('td');var b=document.createElement('button');"
                "b.type='button';b.textContent='Test';b.onclick=function(){fetch('/json/state',{method:'POST',headers:{'Content-Type':'application/json'},"
                "body:'{\"WordClockFx\":{\"wxtest\":'+s+'}}'});};td.appendChild(b);tr.appendChild(td);};};"));
      oappend(F("wcfxledTest=function(tr,cells){var td=document.createElement('td');var b=document.createElement('button');b.type='button';b.textContent='Test';"
                "var cell=cells[1];b.onclick=function(){var ip=cell.querySelector(\"input:not([type='hidden'])\");var v=ip?ip.value:'';if(v==='')return;"
                "fetch('/json/state',{method:'POST',headers:{'Content-Type':'application/json'},body:'{\"WordClockFx\":{\"ledtest\":'+v+'}}'});};td.appendChild(b);tr.appendChild(td);};"));
      oappend(F("wcfxsec('enabled','Display');wcfxsec('showTemperature','Temperature Words');"
                "wcfxsec('fetchWeather','Weather');wcfxsec('useWledLocation','Location');"
                "wcfxsec('weatherPresets','Weather \\u2192 Presets');"));
      // Labels for the non-tabled fields (tabled fields are labelled by their table rows).
      oappend(F("wcfxlbl('enabled','Enabled');wcfxlbl('showPeriodOfDay','Period of day');"
                "wcfxlbl('layout','Layout');"
                "wcfxlbl('fetchWeather','Fetch weather');wcfxlbl('fetchMinutes','Every (min)');"
                "wcfxlbl('weatherPresets','Enable presets');wcfxlbl('heatAbove','Heat above (\\u00B0C)');"
                "wcfxlbl('windAbove','Wind gust above (km/h)');"
                "wcfxlbl('cornerLeds','Light LED on press');wcfxlbl('cornerColor','LED color');"
                "wcfxlbl('minuteDots','Minute Dots');"));
      oappend(F("wcfxsec('cornerLeds','Corner Buttons & LEDs');"));
      // Layout dropdown: scan the FS root for wcfx-*.json, label each option by the
      // file's "name" field; a docs link (the layout's name, from its "link" field)
      // renders right after the dropdown.
      // Only the [file,name,link] entries are runtime-built (sanitized via wcfxJsEntry);
      // head/tail are fixed literals so the quote-balance risk stays contained.
      oappend(F("(function(){var L=["));
      {
        File dir = WLED_FS.open("/", "r");
        bool selSeen = false; uint8_t cnt = 0;
        for (File f = dir.openNextFile(); f && cnt < 24; f = dir.openNextFile()) {
          String fn = f.name();
          if (fn.startsWith("/")) fn = fn.substring(1);
          if (!fn.startsWith(F("wcfx-")) || !fn.endsWith(F(".json"))) continue;
          StaticJsonDocument<64>  filter; filter["name"] = true; filter["link"] = true;
          StaticJsonDocument<384> doc;    // local docs: no WLED JSON-buffer lock needed
          deserializeJson(doc, f, DeserializationOption::Filter(filter));
          const String nm = doc["name"] | fn.c_str();   // no name -> show the filename
          const String lk = doc["link"] | "";
          if (fn == layoutFile) selSeen = true;
          cnt++;
          oappend(wcfxJsEntry(fn, nm, lk).c_str());
        }
        dir.close();
        // Keep a vanished selection visible so saving doesn't silently snap to the
        // first option and rewrite the config.
        if (!selSeen && layoutFile.length())
          oappend(wcfxJsEntry(layoutFile, layoutFile + F(" (missing)"), "").c_str());
      }
      oappend(F("];L.sort(function(a,b){return a[1]<b[1]?-1:a[1]>b[1]?1:0;});"
                "var dd=addDropdown('WordClockFx','layout');if(!dd)return;var m={};"
                "for(var i=0;i<L.length;i++){addOption(dd,L[i][1],L[i][0]);m[L[i][0]]=[L[i][1],L[i][2]];}"
                // docs link: normal-size, link-colored, shows the layout's name + arrow
                "var a=document.createElement('a');a.id='wcfxlink';a.target='_blank';"
                "a.className='wcfxlnk';"
                "dd.parentNode.insertBefore(a,dd.nextSibling);"
                // letter-grid preview: fetch the selected file, dim filler letters
                // (cells no word covers); hidden when the file has no "letters" array.
                "var g=document.createElement('pre');g.id='wcfxgrid';g.className='wcfxcard';"
                "g.style.display='none';var nb=dd.nextSibling;"
                "while(nb&&nb.nodeName!=='BR')nb=nb.nextSibling;"
                "dd.parentNode.insertBefore(g,nb?nb.nextSibling:null);"
                "var u=function(){var e=m[dd.value]||['',''];"
                "a.style.display=e[1]?'':'none';a.href=e[1];a.textContent=e[0]+' \\u2197';"
                "fetch('/'+dd.value).then(function(r){return r.json();}).then(function(j){"
                "if(!j.letters||!j.words){g.style.display='none';return;}"
                "var c={};for(var i=0;i<j.words.length;i++){var w=j.words[i];"
                "for(var k=0;k<w[3];k++)c[w[2]*64+w[1]+k]=1;}"
                "g.innerHTML='';g.style.display='';"
                "for(var y=0;y<j.letters.length;y++){var rw=''+j.letters[y];"
                "for(var x=0;x<rw.length;x++){var s=document.createElement('span');"
                "s.textContent=rw.charAt(x);if(!c[y*64+x])s.className='d';g.appendChild(s);}"
                "g.appendChild(document.createTextNode('\\n'));}"
                "}).catch(function(){g.style.display='none';});};"
                "dd.addEventListener('change',u);u();})();"));
      // ---- tables -------------------------------------------------------------
      oappend(F("wcfxtbl(['Setting','Value'],[['Show temperature words',['showTemperature']],"
                "['Use \\u00B0F (display only)',['fahrenheit']],['Cold below (\\u00B0C)',['coldBelow']],"
                "['Cool below (\\u00B0C)',['coolBelow']],['Warm below (\\u00B0C)',['warmBelow']],"
                "['Manual temp (\\u00B0C)',['manualTemp']]]);"));
      oappend(F("wcfxtbl(['Setting','Value'],[['Use WLED location',['useWledLocation']],"
                "['Place (city/ZIP)',['place']],['Latitude',['latitude']],['Longitude',['longitude']]]);"));
      oappend(F("(function(){var ps=[['Clear','presetClear',1],['Clouds','presetClouds',2],['Fog','presetFog',3],"
                "['Drizzle','presetDrizzle',4],['Rain','presetRain',5],['Snow','presetSnow',6],['Thunder','presetThunder',7],"
                "['Ice','presetIce',8],['Hail','presetHail',9],['Heat','presetHeat',10],['Wind','presetWind',11],['Severe','presetSevere',12]];"
                "var rows=[];for(var i=0;i<ps.length;i++)rows.push([ps[i][0],[ps[i][1]],wcfxwxTest(ps[i][2])]);"
                "wcfxtbl(['Weather','Preset','Test'],rows);})();"));
      oappend(F("(function(){var cr=[['Top-Left',0],['Top-Right',1],['Bottom-Left',2],['Bottom-Right',3]];"
                "var rows=[];for(var i=0;i<cr.length;i++)rows.push([cr[i][0],['cbBtn'+cr[i][1],'cbLed'+cr[i][1]],wcfxledTest]);"
                "wcfxtbl(['Corner','Button','LED','Test'],rows);})();"));
      // ---- field help (after tables so it lands inside the value cells) --------
      oappend(F("addInfo('WordClockFx:enabled', 1, \"<i class='wcfxi'>reboot to apply</i>\");"));
      oappend(F("addInfo('WordClockFx:showPeriodOfDay', 1, \"<i class='wcfxi'>lights IN THE MORNING/AFTERNOON/EVENING, AT NIGHT</i>\");"));
      oappend(F("addInfo('WordClockFx:useWledLocation', 1, \"<i class='wcfxi'>else use Place / lat-lon</i>\");"));
      oappend(F("addInfo('WordClockFx:place', 1, \"<i class='wcfxi'>city or ZIP</i>\");"));
      oappend(F("addInfo('WordClockFx:longitude', 1, \"<i class='wcfxi'><a href='https://www.latlong.net' target='_blank'>find lat/lon</a></i>\");"));
      oappend(F("addInfo('WordClockFx:layout', 1, \"<i class='wcfxi'>add faces: upload wcfx-*.json via /edit; delete a stock file to restore it; position via the segment's 2D bounds</i>\");"));
      oappend(F("addInfo('WordClockFx:cornerLeds', 1, \"<i class='wcfxi'>native WLED buttons; lights mapped LED while held</i>\");"));
      oappend(F("addInfo('WordClockFx:cornerColor', 1, \"<i class='wcfxi'>hex RGB or RGBW</i>\");"));
      oappend(F("addInfo('WordClockFx:minuteDots', 1, \"<i class='wcfxi'>corner LEDs count the minutes a 5-minute layout can't show (minute % 5); 5-minute layouts only</i>\");"));

      // ---- live status panel + "Update now" -----------------------------------
      oappend(F("addInfo('WordClockFx:fetchWeather', 1, \"<div id='wcfxstat'>loading current weather...</div>"
                "<div class='wcfxi' style='margin:2px 14px 3px'>Weather data by "
                "<a href='https://open-meteo.com' target='_blank'>open-meteo.com</a><span id='wcfxsrc'></span></div>\");"));
      // The card+attribution are injected between the 'Fetch weather' checkbox and its trailing
      // <br> (WLED renders each field as 'label <input> ... <br>'). Drop that now-empty <br> so the
      // 'Every (min)' row sits snug under the attribution instead of after a blank line.
      oappend(F("(function(){var c=document.getElementById('wcfxstat');if(!c)return;"
                "for(var n=c.nextSibling;n;n=n.nextSibling){if(n.nodeName==='BR'){n.parentNode.removeChild(n);break;}}})();"));
      oappend(F("wcfxrefresh=function(){fetch('/json/info').then(function(r){return r.json();}).then(function(j){"
                "var u=(j&&j.u)||{};function g(k){var a=u[k];return a?(Array.isArray(a)?a.join(''):a):'-';}"
                "var e=document.getElementById('wcfxstat');if(!e)return;var src=g('Word Clock source');"
                "e.innerHTML='&#127777;&#65039; '+g('Word Clock temperature')+' &nbsp; &#128167; '+g('Word Clock humidity')+"
                "' &nbsp; &#128168; '+g('Word Clock wind gust')+"
                "' &nbsp; '+g('Word Clock condition')+'<br>&#128205; '+g('Word Clock location')+"
                "' &nbsp; &#128260; '+g('Word Clock updated');"
                "var sp=document.getElementById('wcfxsrc');if(sp){var hm=(src!=='-')?src.match(/href=\"([^\"]+)\"/):null;"
                "sp.innerHTML=hm?(' &middot; <a href=\"'+hm[1]+'\" target=\"_blank\">view source</a>'):'';}"
                "}).catch(function(){"
                "var e=document.getElementById('wcfxstat');if(e)e.innerHTML='(status unavailable)';});};"));
      oappend(F("wcfxupd=function(){var e=document.getElementById('wcfxstat');if(e)e.innerHTML='Updating...';"
                "fetch('/json/state',{method:'POST',headers:{'Content-Type':'application/json'},body:'{\"WordClockFx\":{\"update\":true}}'})"
                ".then(function(){setTimeout(wcfxrefresh,3000);setTimeout(wcfxrefresh,7000);}).catch(function(){var e=document.getElementById('wcfxstat');if(e)e.innerHTML='request failed';});};"));
      oappend(F("addInfo('WordClockFx:fetchMinutes', 1, \"&nbsp;<button type='button' onclick='wcfxupd()'>Update now</button>\");"));
      oappend(F("setTimeout(wcfxrefresh,300);"));

      // ---- per-state preset dropdowns (populated from /presets.json) -----------
      oappend(F("(function(){function f(j){var ks=['presetClear','presetClouds','presetFog','presetDrizzle','presetRain','presetSnow','presetThunder','presetIce','presetHail','presetHeat','presetWind','presetSevere'];"
                "for(var i=0;i<ks.length;i++){var dd=addDropdown('WordClockFx',ks[i]);if(!dd)continue;addOption(dd,'None',0);"
                "for(var p of Object.entries(j)){if(p[0]==='0')continue;var n=(p[1]&&p[1].n)?p[1].n:('Preset '+p[0]);addOption(dd,p[0]+': '+n,p[0]);}}}"
                "fetch('/presets.json').then(function(r){return r.json();}).then(f).catch(function(){});})();"));

    }

    // No getId() override: this usermod needs no unique id (it isn't detected by other
    // usermods and exchanges no um_data), so it keeps USERMOD_ID_UNSPECIFIED and avoids
    // touching wled00/const.h. See the note at the USERMOD_ID list in const.h.
};

const char WordClockFxUsermod::_name[]        PROGMEM = "WordClockFx";
const char WordClockFxUsermod::_enabled[]     PROGMEM = "enabled";
const char WordClockFxUsermod::_showPeriod[]  PROGMEM = "showPeriodOfDay";
const char WordClockFxUsermod::_layout[]      PROGMEM = "layout";
const char WordClockFxUsermod::_minuteDots[]  PROGMEM = "minuteDots";
const char WordClockFxUsermod::_showTemp[]    PROGMEM = "showTemperature";
const char WordClockFxUsermod::_fahrenheit[]  PROGMEM = "fahrenheit";
const char WordClockFxUsermod::_thrColdCool[] PROGMEM = "coldBelow";
const char WordClockFxUsermod::_thrCoolWarm[] PROGMEM = "coolBelow";
const char WordClockFxUsermod::_thrWarmHot[]  PROGMEM = "warmBelow";
const char WordClockFxUsermod::_manualTemp[]  PROGMEM = "manualTemp";
const char WordClockFxUsermod::_fetch[]       PROGMEM = "fetchWeather";
const char WordClockFxUsermod::_useWled[]     PROGMEM = "useWledLocation";
const char WordClockFxUsermod::_interval[]    PROGMEM = "fetchMinutes";
const char WordClockFxUsermod::_place[]       PROGMEM = "place";
const char WordClockFxUsermod::_lat[]         PROGMEM = "latitude";
const char WordClockFxUsermod::_lon[]         PROGMEM = "longitude";
const char WordClockFxUsermod::_presets[]     PROGMEM = "weatherPresets";
const char WordClockFxUsermod::_heatAbove[]   PROGMEM = "heatAbove";
const char WordClockFxUsermod::_windAbove[]   PROGMEM = "windAbove";
// Per-state preset keys live in WCFX_PRESET_KEYS[] (file scope, near the WxState enum).

static WordClockFxUsermod usermod_word_clock_fx;
REGISTER_USERMOD(usermod_word_clock_fx);
