#!/usr/bin/env python3
"""Driver for wcfx_validate.py: validate every 16x16 exact-minute face in layouts/.

Run after ANY layout edit (see layouts/wcfx-16x16-mk3-handoff.md section 6):

    python3 tools/wcfx_validate_run.py

Loads each layouts/wcfx-16x16-*.json (exact grammar only — the validator models the
exact-minute sentence builder; the 11x10 five-minute face is out of scope), converts
its words list to the validator's {role: (x, y, len)} form, and enforces C1-C4 across
all 1440 minutes in both period modes. Exits non-zero if any face fails.
"""
import glob
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

from wcfx_validate import validate  # noqa: E402

# Intentional same-row fusions (no dark cell between): '&' + COLD, a symbol boundary.
FUSIONS = frozenset({("and", "cold")})

ok_all = True
for path in sorted(glob.glob(os.path.join(ROOT, "layouts", "wcfx-16x16-*.json"))):
    doc = json.load(open(path))
    if doc.get("grammar") != "exact":
        continue
    words = {}
    for role, x, y, ln in doc["words"]:
        if role in words:
            print(f"FAIL {os.path.basename(path)}: duplicate role {role!r} "
                  "(validator expects one entry per role)")
            ok_all = False
        words[role] = (x, y, ln)
    connector = "until" if "until" in words else "to"
    # Only demand AM/PM coverage of faces that have the tiles (the firmware treats
    # missing roles as silent no-ops, so a face without them is valid — e.g. MK1).
    modes = ("words", "ampm") if ("am" in words and "pm" in words) else ("words",)
    passed, spare = validate(os.path.basename(path), doc["letters"], words,
                             fusions=FUSIONS, period_modes=modes,
                             connector=connector)
    ok_all &= passed

sys.exit(0 if ok_all else 1)
