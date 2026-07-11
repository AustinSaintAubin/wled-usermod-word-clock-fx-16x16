#!/usr/bin/env python3
"""wcfx_validate.py - validator/scorer for Word Clock FX exact-minute 16x16 faces.

Hard constraints (pass/fail):
  C1 coverage      - every role the exact grammar can emit exists in the face
  C2 letters match - each word's coordinates spell the expected text on the grid
  C3 reading order - for all 1440 minutes, lit words appear in strict row-major
                     reading order (fusions allowed: adjacency still increases)
  C4 separation    - co-lit words on the same row have >=1 dark cell between
                     them, except whitelisted intentional fusions

Quality metrics:
  spare cells, avg/min/max lit LEDs, per-cell usage heatmap summary
"""
import json, sys
from collections import defaultdict

W = H = 16

ROLE_TEXT = {
    "it": "IT", "is": "IS", "a": "A", "quarter": "QUARTER", "half": "HALF",
    "past": "PAST", "until": "UNTIL", "oclock": "OCLOCK", "minutes": "MINUTES",
    "am": "AM", "pm": "PM", "in": "IN", "the": "THE", "at": "AT",
    "morning": "MORNING", "afternoon": "AFTERNOON", "evening": "EVENING",
    "night": "NIGHT", "and": "&", "cold": "COLD", "cool": "COOL",
    "warm": "WARM", "hot": "HOT", "midnight": "MIDNIGHT", "to": "TO",
}
NUM = ["", "ONE", "TWO", "THREE", "FOUR", "FIVE", "SIX", "SEVEN", "EIGHT",
       "NINE", "TEN", "ELEVEN", "TWELVE", "THIRTEEN", "FOURTEEN", "FIFTEEN",
       "SIXTEEN", "SEVENTEEN", "EIGHTEEN", "NINETEEN", "TWENTY"]
for i in range(1, 21):
    ROLE_TEXT[f"m{i}"] = NUM[i]
for i in range(1, 13):
    ROLE_TEXT[f"h{i}"] = NUM[i]


def minute_roles(m):
    if m <= 20:
        return [f"m{m}"]
    return ["m20", f"m{m - 20}"]


def sentence(t, period_mode="words", connector="until"):
    """Ordered role sequence for minute-of-day t, per the documented grammar."""
    h24, m = divmod(t, 60)
    if h24 == 0 and m == 0:
        return ["it", "is", "midnight"]          # standalone by design
    seq = ["it", "is"]
    if m == 0:
        hour = h24 % 12 or 12
        seq += [f"h{hour}", "oclock"]
    elif m <= 30:
        hour = h24 % 12 or 12
        if m == 15:
            seq += ["a", "quarter", "past"]
        elif m == 30:
            seq += ["half", "past"]
        else:
            seq += minute_roles(m) + ["minutes", "past"]
        seq += [f"h{hour}"]
    else:
        mm = 60 - m
        hour = (h24 + 1) % 12 or 12
        if mm == 15:
            seq += ["a", "quarter", connector]
        else:
            seq += minute_roles(mm) + ["minutes", connector]
        seq += [f"h{hour}"]
    if period_mode == "words":
        if h24 <= 11:
            seq += ["in", "the", "morning"]
        elif h24 <= 16:
            seq += ["in", "the", "afternoon"]
        elif h24 <= 20:
            seq += ["in", "the", "evening"]
        else:
            seq += ["at", "night"]
    elif period_mode == "ampm":
        seq += ["am" if h24 < 12 else "pm"]
    return seq


def validate(name, grid, words, fusions=frozenset(), period_modes=("words",), connector="until"):
    print(f"\n=== {name} ===")
    errs = []
    assert len(grid) == H and all(len(r) == W for r in grid), "grid must be 16x16"

    # C2: coordinates spell the right text
    for role, (x, y, ln) in words.items():
        got = grid[y][x:x + ln]
        want = ROLE_TEXT[role]
        if got != want:
            errs.append(f"C2 {role}: expected {want!r}, grid has {got!r} at ({x},{y})")

    # C1 + C3 + C4 across all minutes (plus '&'+temp ordering once)
    usage = defaultdict(int)
    lit_counts = []
    for mode in period_modes:
        for t in range(1440):
            seq = sentence(t, mode, connector)
            missing = [r for r in seq if r not in words]
            if missing:
                errs.append(f"C1 t={t} mode={mode}: missing roles {missing}")
                continue
            spans = []
            lit = 0
            for r in seq:
                x, y, ln = words[r]
                spans.append((r, y * W + x, y * W + x + ln - 1, y))
                lit += ln
                for i in range(ln):
                    usage[(x + i, y)] += 1
            if mode == period_modes[0]:
                lit_counts.append(lit)
            for (r1, s1, e1, y1), (r2, s2, e2, y2) in zip(spans, spans[1:]):
                if s2 <= e1:
                    errs.append(f"C3 t={t}: {r2} does not follow {r1} in reading order")
                elif y1 == y2 and s2 - e1 - 1 < 1 and (r1, r2) not in fusions:
                    errs.append(f"C4 t={t}: {r1}+{r2} co-lit with no gap")

    # temperature ordering (& precedes each temp word)
    ax, ay, aln = words["and"]
    for temp in ("cold", "cool", "warm", "hot"):
        if temp not in words:
            continue
        tx, ty, _ = words[temp]
        if ty * W + tx <= ay * W + ax + aln - 1:
            errs.append(f"C3 temp: {temp} not after '&'")

    covered = {(words[r][0] + i, words[r][1]) for r in words for i in range(words[r][2])}
    spare = W * H - len(covered)
    dedup = sorted(set(errs))
    print(f"result: {'PASS' if not dedup else 'FAIL'}  "
          f"({len(dedup)} unique issues)")
    for e in dedup[:12]:
        print("  !", e)
    print(f"spare cells (no word coverage): {spare}/256")
    print(f"lit LEDs per sentence: avg {sum(lit_counts)/len(lit_counts):.1f}, "
          f"min {min(lit_counts)}, max {max(lit_counts)}")
    rows_used = defaultdict(int)
    for (x, y), n in usage.items():
        rows_used[y] += n
    top = max(rows_used.values())
    print("row usage share:", " ".join(f"{rows_used[y]*100//top:3d}" for y in range(H)))
    return not dedup, spare


def render(grid, words, t, mode="words", connector="until"):
    seq = sentence(t, mode, connector)
    lit = set()
    for r in seq:
        x, y, ln = words[r]
        lit |= {(x + i, y) for i in range(ln)}
    out = []
    for y in range(H):
        out.append(" ".join(grid[y][x] if (x, y) in lit else "\u00b7" for x in range(W)))
    return "\n".join(out)
