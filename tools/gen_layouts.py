#!/usr/bin/env python3
"""Embed layouts/*.json into layouts/_wcfx_layouts.generated.h (single source of truth).

This script lives in tools/; the JSON faces and the generated header live in the
sibling layouts/ directory. Runs two ways:
- As a PlatformIO library extraScript (declared in library.json "build" as
  "tools/gen_layouts.py") — PIO executes it as an SConscript before compiling
  the usermod, so the header is always regenerated from the layout files, for
  both symlink dev checkouts and git-fetched custom_usermods copies.
- Standalone: `python3 tools/gen_layouts.py` (used by the host test harness).

A malformed layout file fails the build with a clear error (both invalid JSON and
structurally invalid words/letters). The header is only rewritten when its content
changes, so unchanged layouts don't trigger rebuilds.
"""
import glob
import json
import os
import sys

_env = None
try:  # SCons/PlatformIO context (extraScript); harmless when standalone
    Import("env")  # noqa: F821  (SCons builtin)
    _env = env     # noqa: F821
except Exception:
    pass

try:  # this file lives in tools/, a sibling of layouts/
    _here = os.path.dirname(os.path.abspath(__file__))
except NameError:
    # SCons exec's this file without __file__. PlatformIO chdir's to the extraScript's
    # directory (this tools/ folder); older/other setups may leave cwd at the lib root.
    _here = os.getcwd()
if os.path.basename(_here) in ("tools", "layouts"):
    LAYOUT_DIR = os.path.join(os.path.dirname(_here), "layouts")
else:  # lib root
    LAYOUT_DIR = os.path.join(_here, "layouts")
OUT = os.path.join(LAYOUT_DIR, "_wcfx_layouts.generated.h")

WCFX_MAX_W = 32
WCFX_MAX_H = 32


def die(msg):
    sys.stderr.write("gen_layouts.py: ERROR: %s\n" % msg)
    raise SystemExit(1)


def c_escape(line):
    return line.replace("\\", "\\\\").replace('"', '\\"')


# Structural validation: catch bad stock-layout edits at build time instead of
# letting the device fall back to the embedded 16x16 at runtime. Role tokens are
# NOT checked here (that list lives in the C++ and would drift); the device parse
# reports an unknown role. Mirrors the bounds checks in parseLayoutDoc().
def validate(name, doc):
    w, h = doc["width"], doc["height"]
    if not isinstance(w, int) or not isinstance(h, int) or not (1 <= w <= WCFX_MAX_W) or not (1 <= h <= WCFX_MAX_H):
        die("layouts/%s: width/height must be ints in 1..%d (got %rx%r)" % (name, WCFX_MAX_W, w, h))
    if doc["grammar"] not in ("five", "exact"):
        die("layouts/%s: grammar must be 'five' or 'exact' (got %r)" % (name, doc["grammar"]))
    words = doc["words"]
    if not isinstance(words, list) or not (1 <= len(words) <= 96):
        die("layouts/%s: 'words' must be an array of 1..96 entries" % name)
    for i, e in enumerate(words):
        if (not isinstance(e, list) or len(e) != 4 or not isinstance(e[0], str)
                or not all(isinstance(v, int) for v in e[1:])):
            die("layouts/%s: word %d must be [role:str, x:int, y:int, len:int] (got %r)" % (name, i, e))
        role, x, y, ln = e
        if x < 0 or y < 0 or ln < 1 or x + ln > w or y >= h:
            die("layouts/%s: word %d ('%s') out of bounds: x=%d y=%d len=%d in %dx%d"
                % (name, i, role, x, y, ln, w, h))
    letters = doc.get("letters")
    if letters is not None:
        if not isinstance(letters, list) or len(letters) != h:
            die("layouts/%s: 'letters' must have exactly height (%d) rows (got %d)"
                % (name, h, len(letters) if isinstance(letters, list) else -1))
        for y, row in enumerate(letters):
            if not isinstance(row, str) or len(row) != w:
                die("layouts/%s: letters row %d must be a string of width %d (got %r)" % (name, y, w, row))


files = sorted(glob.glob(os.path.join(LAYOUT_DIR, "*.json")))
if not files:
    die("no layout files found in %s" % LAYOUT_DIR)

default_file = None  # exactly one layout must carry "default": true

out = [
    "// AUTO-GENERATED from layouts/*.json by gen_layouts.py - DO NOT EDIT.",
    "// Edit the JSON files in layouts/ instead; this header is regenerated at build.",
    "#pragma once",
    "",
    "struct WcfxEmbeddedLayout { const char *path; const char *json; };",
    "",
]
entries = []
for i, path in enumerate(files):
    name = os.path.basename(path)
    try:
        with open(path, "r") as f:
            text = f.read()
        doc = json.loads(text)
    except Exception as e:
        die("layouts/%s is not valid JSON: %s" % (name, e))
    for key in ("width", "height", "grammar", "words"):
        if key not in doc:
            die("layouts/%s is missing required key '%s'" % (name, key))
    validate(name, doc)
    if "default" in doc:
        if not isinstance(doc["default"], bool):
            die("layouts/%s: 'default' must be true/false (got %r)" % (name, doc["default"]))
        if doc["default"]:
            if default_file is not None:
                die('both layouts/%s and layouts/%s claim "default": true - only one layout may'
                    % (default_file, name))
            default_file = name
    var = "WCFX_EMBEDDED_JSON_%d" % i
    out.append("// layouts/%s" % name)
    out.append("static const char %s[] PROGMEM =" % var)
    lines = text.split("\n")
    while lines and lines[-1] == "":  # normalize a trailing newline away
        lines.pop()
    for j, line in enumerate(lines):
        nl = "" if j == len(lines) - 1 else "\\n"
        out.append('  "%s%s"' % (c_escape(line), nl))
    out[-1] += ";"
    out.append("")
    entries.append('  { "/%s", %s },' % (name, var))

out.append("static const WcfxEmbeddedLayout WCFX_EMBEDDED[] = {")
out.extend(entries)
out.append("};")
out.append("")

if default_file is None:
    die('no layout claims "default": true - mark exactly one (it becomes the firmware '
        "default face, fallback, and config-migration target)")
out.append('// The layout marked "default": true - firmware default face + fallback.')
out.append('#define WCFX_DEFAULT_LAYOUT_FILE "%s"' % default_file)
out.append('#define WCFX_DEFAULT_LAYOUT_PATH "/%s"' % default_file)
out.append("")
content = "\n".join(out)

old = None
if os.path.exists(OUT):
    with open(OUT, "r") as f:
        old = f.read()
if old != content:
    with open(OUT, "w") as f:
        f.write(content)
    print("gen_layouts.py: wrote %s (%d layouts)" % (os.path.basename(OUT), len(files)))

# Opt-in deep validation (the 1,440-minute face validator, manual by default).
# Enable per build with the env var WCFX_VALIDATE=1, or persistently in
# platformio_override.ini with:  custom_wcfx_validate = true
_want = os.environ.get("WCFX_VALIDATE", "")
if not _want and _env is not None:
    try:
        _want = _env.GetProjectOption("custom_wcfx_validate", "")
    except Exception:
        _want = ""
if str(_want).strip().lower() in ("1", "true", "yes", "on"):
    import subprocess
    runner = os.path.join(os.path.dirname(LAYOUT_DIR), "tools", "wcfx_validate_run.py")
    print("gen_layouts.py: running face validator (opt-in)...")
    if subprocess.call([sys.executable, runner]) != 0:
        die("face validation failed (see report above)")
