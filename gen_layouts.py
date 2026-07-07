#!/usr/bin/env python3
"""Embed layouts/*.json into wcfx_layouts.generated.h (single source of truth).

Runs two ways:
- As a PlatformIO library extraScript (declared in library.json "build") — PIO
  executes it as an SConscript before compiling the usermod, so the header is
  always regenerated from the layout files, for both symlink dev checkouts and
  git-fetched custom_usermods copies.
- Standalone: `python3 gen_layouts.py` (used by the host test harness).

A malformed layout file fails the build with a clear error. The header is only
rewritten when its content changes, so unchanged layouts don't trigger rebuilds.
"""
import glob
import json
import os
import sys

try:  # SCons/PlatformIO context (extraScript); harmless when standalone
    Import("env")  # noqa: F821  (SCons builtin)
except Exception:
    pass

try:
    ROOT = os.path.dirname(os.path.abspath(__file__))
except NameError:  # SCons exec's this file without __file__; chdir'd to the lib root
    ROOT = os.getcwd()
LAYOUT_DIR = os.path.join(ROOT, "layouts")
OUT = os.path.join(ROOT, "wcfx_layouts.generated.h")


def die(msg):
    sys.stderr.write("gen_layouts.py: ERROR: %s\n" % msg)
    raise SystemExit(1)


def c_escape(line):
    return line.replace("\\", "\\\\").replace('"', '\\"')


files = sorted(glob.glob(os.path.join(LAYOUT_DIR, "*.json")))
if not files:
    die("no layout files found in %s" % LAYOUT_DIR)

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
content = "\n".join(out)

old = None
if os.path.exists(OUT):
    with open(OUT, "r") as f:
        old = f.read()
if old != content:
    with open(OUT, "w") as f:
        f.write(content)
    print("gen_layouts.py: wrote %s (%d layouts)" % (os.path.basename(OUT), len(files)))
