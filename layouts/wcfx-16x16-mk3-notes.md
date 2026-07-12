# WCFX MK3 — 16×16 Face: Handoff, Design Notes & Update Instructions

This is the single source of truth for the MK3 face iteration of the
**Word Clock FX** WLED usermod
(`wled-usermod-word-clock-fx-16x16`). It is written to be consumed by a
human *or* a coding agent tasked with integrating the face into the
repository, updating the README, and deploying to hardware.

**Deliverables accompanying this document**

| File | Purpose |
| --- | --- |
| `wcfx-16x16-mk3-until.json` | MK3 face, UNTIL phrasing ("TEN MINUTES UNTIL EIGHT") |
| `wcfx-16x16-mk3-to.json` | MK3 face, TO phrasing ("TEN MINUTES TO EIGHT") |
| `wcfx_validate.py` | Validator/scorer — re-run after ANY layout edit (see §6) |

Filename convention (use for all future faces):
`wcfx-{size}-{iteration}-{variant}.json` → e.g. `wcfx-16x16-mk3-until.json`.

---

## 1. README-ready summary (paste-adaptable)

> ### MK3 Face (16×16, exact-minute)
>
> The MK3 face is a machine-validated redesign of the MK2 layout with a
> **dual connector**: both UNTIL and TO are printed on the panel, and two
> otherwise-identical layout files let you switch phrasing from the Layout
> dropdown — no reflash, no new faceplate. The face supports both
> period-of-day display modes (MORNING/AFTERNOON/EVENING/AT NIGHT words
> **and** AM/PM tiles) plus the full temperature scale
> (`& COLD / COOL / WARM / HOT`).
>
> Every one of the 1,440 minutes of the day was verified programmatically
> for word coverage, strict reading order, and lit-word separation. Letter
> economy comes from shared-letter chains (`FIVELEVEN`, `EIGHTEN`,
> `SEVENINE`, `TWONE`) and embedded words (FOUR inside FOURTEEN, SIX/SEVEN/
> EIGHT/NINE inside their teens). MIDNIGHT now sits below the connectors,
> removing the MK2 grammar workaround. The MK2 `UNTILL` typo is gone.
>
> The dark letters are not random: the panel hides KISS, PASTIMES,
> ACQUARTER, ENDURE, `UNTIL TO TOO`, `OCLOCK WIND THE CAT`, the designer's
> initials, and a reserved **IT IS DONE** phrase awaiting a future firmware
> role. See the design notes for the full list.

```
I T K I S S T W E N T Y D O N E
T H I R T E E N F O U R T E E N
S I X T E E N N I N E T E E N D
S E V E N T E E N E L E V E N U
E I G H T E E N T W O F I V E R
T W E L V E T E N T H R E E B E
A C Q U A R T E R H A L F A S A
M I N U T E S O P A S T I M E S
U N T I L B T O O F O U R S I X
F I V E L E V E N E I G H T E N
S E V E N I N E M I D N I G H T
T W E L V E T H R E E T W O N E
O C L O C K W I N D T H E C A T
N I G H T A M P M M O R N I N G
A F T E R N O O N E V E N I N G
& C O L D C O O L W A R M H O T
```

---

## 2. Repository integration (coding-agent task list)

1. Copy `wcfx-16x16-mk3-until.json` and `wcfx-16x16-mk3-to.json` into the
   repo's `layouts/` directory so they compile in and seed to the
   filesystem like the existing stock faces.
2. Update the README: replace/extend the MK2 layout section with §1 above
   and the updated layout table (the letter grid). Note the dual-connector
   mechanism explicitly — it is the headline feature.
3. If stock-face seeding is list-based in the usermod source, append both
   new filenames to that list. Keep the JSON `name` fields
   (`"MK3 16x16 (until)"`, `"MK3 16x16 (to)"`) — they sort adjacently in
   the Layout dropdown.
4. Do NOT modify word coordinates without re-running the validator (§6).
   Filler letters (cells not covered by any `words` entry) may be changed
   freely for panel-printing purposes.

## 3. Deploying to a running clock (no reflash path)

1. Open the WLED file editor at `http://<clock-ip>/edit`.
2. Upload both JSON files.
3. In the usermod settings (Config → Usermods → Word Clock FX), set the
   Layout to `MK3 16x16 (until)` or `MK3 16x16 (to)` — this choice IS the
   phrasing setting.
4. Trigger `reloadLayout` (usermod button/setting) or reboot.
5. Smoke test, minimum set: 12:00 (`TWELVE OCLOCK`), 00:00
   (`IT IS MIDNIGHT`), 12:21 (`TWENTY ONE MINUTES PAST TWELVE`), xx:15
   (`A QUARTER PAST`), xx:30 (`HALF PAST`), xx:45 (`A QUARTER UNTIL/TO`),
   21:40 (`TWENTY MINUTES UNTIL/TO TEN AT NIGHT`), plus AM/PM display mode
   on any time. Confirm temperature row lights if the temp source is
   configured.

## 4. Face specification

Grid: 16×16, exact-minute grammar. Words are horizontal `[role, x, y, len]`
entries, `(x, y)` zero-indexed from top-left. The two JSON files are the
authoritative word maps; they are identical except the connector:
`["until", 0, 8, 5]` vs `["to", 6, 8, 2]`.

Band structure (rows 1-based): row 1 IT/IS + TWENTY + ONE; rows 2–6 minute
words 2–19 (teens embed their units: FOUR⊂FOURTEEN, SIX⊂SIXTEEN,
SEVEN⊂SEVENTEEN, EIGHT⊂EIGHTEEN, NINE⊂NINETEEN); row 7 A QUARTER / HALF;
row 8 MINUTES / PAST; row 9 UNTIL / TO / hours FOUR+SIX; rows 10–12
remaining hours via shared-letter chains + MIDNIGHT; row 13 OCLOCK / IN /
THE / AT; row 14 NIGHT / AM / PM / MORNING; row 15 AFTERNOON / EVENING;
row 16 `&` + COLD / COOL / WARM / HOT.

Chains: `FIVELEVEN` (FIVE+ELEVEN share E), `EIGHTEN` (EIGHT+TEN share T),
`SEVENINE` (SEVEN+NINE share N), `TWONE` (TWO+ONE share O). Rows 2, 10,
11, 12, 14, 15, 16 contain zero wasted cells.

Compound minutes: :21–:29 light m20 + the unit word with a guaranteed gap
or row break (e.g. `TWENTY · ONE` on row 1 with the dark D between).

## 5. Easter eggs & reserved words

| Item | Where (x,y) | Story / status |
| --- | --- | --- |
| **KISS** | (2,0)–(5,0) | Filler K and S wrap the functional `IS`; panel prints `ITKISS`. MK2 heritage; *Keep It Stupid Simple* is the house philosophy. Never lights. |
| **IT IS DONE** | reserved `done` = (12,0,4) | Filler D + functional ONE spell `DONE`. Not defined in the face files. Future firmware role idea: timer-complete / notification phrase — `IT IS DONE`. Reading order (IT < IS < DONE) already valid. |
| **NOON** | reserved `noon` = (5,14,4) | Free — NOON sits inside the functional AFTERNOON. Reading order verified for `IT IS NOON`, `HALF PAST NOON`, and `<minutes> UNTIL/TO NOON`. A future `noon` role completes the MIDNIGHT symmetry at zero layout cost. |
| **ENDURE** | column 15 (x=15), rows 0–5, reading down | E and N borrowed from the functional ONE and FOURTEEN; D-U-R-E are fillers. The clock endures. Never lights. |
| **PASTIMES** | row (8,7)–(15,7) | Fillers I-M-E-S after the functional `PAST`; row prints `MINUTES PASTIMES`. |
| **ACQUARTER** | (0,6)–(8,6) | Filler C between `A` and `QUARTER` — homage to the classic WordClock 2022 11×10 stock face. |
| **ASA** | (13,6)–(15,6) | Designer's initials — Austin St. Aubin. |
| **UNTIL, TO, TOO** | row 9 | Both functional connectors plus a filler O completing `TOO`: every way to say it on one row. |
| **OCLOCK WIND THE CAT** | row 13 | Fillers W, D, C wrap the functional `IN`/`THE`/`AT` — the old joke about putting out the clock and winding the cat. Cells (6,12) and (9,12) are mandatory dark separators; (13,12) is the row's only true spare and never co-lights with anything, making it a candidate firmware status pixel (Wi-Fi loss, stale temp source, notification) regardless of the printed letter. Print-time alternative: W-I-N-E at (6..9,12) enables a novelty `wine` role — a period-phrase override rendering `IT IS FIVE OCLOCK WINE` — but it cannot coexist with `IN THE …` phrases, so it is an override only. |
| **SEVENINEMIDNIGHT** | row 11 | Emergent, not planted: the SEVEN/NINE chain beside MIDNIGHT. Kept with pride. |

Shelved exploration: a variant traded AM/PM + WARM for reserved `VERY` and
`ICY` weather words. The bottom band has zero slack, so post-period
vocabulary always evicts working features on 16×16 — larger-matrix (MK4)
material. The packing math lives in the project chat history and the
validator makes re-exploration cheap.

## 6. Validation — run this after ANY layout change

```
python3 wcfx_validate.py   # module; drive it like faces_final.py does
```

Hard constraints enforced across all 1,440 minutes × both period modes:
C1 every emitted role exists; C2 word coordinates spell their text on the
grid; C3 strict row-major reading order for every sentence; C4 co-lit
same-row words keep ≥1 dark cell (explicit fusion whitelist: `&`+COLD,
a symbol boundary). Expected result for both MK3 files: `PASS (0 unique
issues)`; spares 24/256 (until file) and 27/256 (to file); avg lit LEDs
~38/~37.

Rules for future edits an agent must not break: words are horizontal only;
minute band must precede MINUTES, which precedes PAST/the connector, which
precede all hours, which precede OCLOCK/AM/PM/periods, which precede `&` +
temperature; `A` before `QUARTER`; AT before NIGHT; keep MIDNIGHT below
the connectors (this is what allows future `PAST MIDNIGHT` phrasing).
Bonus latent feature: a future singular `minute` role is the existing
MINUTES tile at length 6 — no layout change needed.
