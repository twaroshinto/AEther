#!/bin/sh
# SPDX-License-Identifier: MPL-2.0
# Copyright (C) 2026 fm1-aether contributors
# AI-generated code (Claude, Anthropic): no warranty, use at your own risk (see NOTICE).
# AEther's site (GitHub Pages): fm1-base's make_site.py (the front page, the web installer, the emulator, the
# firmware), with the manuals from docs/ (Polish in manual/, English in manual/en.html) and the editor (web/editor).
#   tools/make_site.sh VERSION PACKAGE.fwsc OUT_DIR       (app/build/emu must hold the emulator: ./build.sh --web)
set -e
V="${1:?usage: tools/make_site.sh VERSION PACKAGE.fwsc OUT_DIR}"
PKG="${2:?}"
OUT="${3:?}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PY="${PYTHON:-python3}"
# make_site.py takes the manual from APP_DIR/docs: a copy of the Polish one, for the build only
rm -rf "$ROOT/app/docs"
mkdir -p "$ROOT/app/docs"
cp "$ROOT/docs/MANUAL.md" "$ROOT/app/docs/MANUAL.md"
cp -r "$ROOT/docs/img" "$ROOT/app/docs/img"
"$PY" "$ROOT/base/tools/make_site.py" "$ROOT/app" "$PKG" "$V" "$OUT"
rm -rf "$ROOT/app/docs"
# the English manual beside it (its images are the same: manual/img)
"$PY" "$ROOT/base/tools/manual_html.py" "$ROOT/docs/MANUAL.en.md" "$OUT/manual/en.html" --name AETHER \
    --repo https://github.com/twaroshinto/AEther --tagline "a PaulStretch-style spectral synthesizer for the FM-1"
# the manuals link each other as .md files in the repository: as pages here
"$PY" - "$OUT/manual/index.html" "$OUT/manual/en.html" <<'PY'
import sys
for p in sys.argv[1:]:
    s = open(p, encoding="utf-8").read()
    s = s.replace('href="MANUAL.en.md"', 'href="en.html"').replace('href="MANUAL.md"', 'href="index.html"')
    open(p, "w", encoding="utf-8").write(s)
PY
# the editor, when there is one
if [ -d "$ROOT/web/editor" ]; then
    mkdir -p "$OUT/editor"
    cp -r "$ROOT/web/editor/." "$OUT/editor/"
    [ -f "$ROOT/app/build/host/params.json" ] && cp "$ROOT/app/build/host/params.json" "$OUT/editor/params.json"
fi
echo "site: $OUT"
