# SPDX-License-Identifier: MPL-2.0
# Copyright (C) 2026 fm1-aether contributors
# AI-generated code (Claude, Anthropic): no warranty, use at your own risk (see NOTICE).
"""The site in one look: every page gets the fonts, web/site/aether.css (last in its head, so it wins over the
page's own style) and the same bar of links at the top (the current page marked).
   python3 tools/site_theme.py SITE_DIR"""
import re
import shutil
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
site = Path(sys.argv[1])
shutil.copy(ROOT / "web" / "site" / "aether.css", site / "aether.css")

FONTS = ('<link rel="preconnect" href="https://fonts.googleapis.com">'
         '<link rel="preconnect" href="https://fonts.gstatic.com" crossorigin>'
         '<link rel="stylesheet" href="https://fonts.googleapis.com/css2?family=Barlow:ital,wght@0,400;0,600;1,400'
         '&family=Barlow+Semi+Condensed:wght@600;700&display=swap">')
LINKS = [("install/", "Install"), ("emu/", "Play in the browser"), ("editor/", "Editor"), ("manual/", "Manual")]


def bar(rel, here):
    cur = ' aria-current="page"'
    links = "".join(f'<a href="{rel}{href}"{cur if here == href else ""}>{name}</a>' for href, name in LINKS)
    return (f'<header class="ae-bar"><div class="in"><a class="mark" href="{rel or "./"}"><i></i>AETHER</a>'
            f'<nav aria-label="AEther">{links}</nav><span class="grow"></span>'
            f'<nav><a class="src" href="https://github.com/twaroshinto/AEther">Source</a></nav></div></header>')


n = 0
for page in sorted(site.rglob("*.html")):
    rel_dir = page.parent.relative_to(site)
    depth = 0 if str(rel_dir) == "." else len(rel_dir.parts)
    rel = "../" * depth
    here = f"{rel_dir.parts[0]}/" if depth else ""
    s = page.read_text(encoding="utf-8")
    if "ae-bar" in s or "<head" not in s:
        continue
    head = ("" if "Barlow+Semi+Condensed" in s else FONTS) + f'<link rel="stylesheet" href="{rel}aether.css">'
    s = s.replace("</head>", head + "\n</head>", 1)
    # the bar: after the skip link when there is one, else first in the body
    m = re.search(r'<body[^>]*>\s*(<a class="skip"[^>]*>.*?</a>)?', s, flags=re.S)
    if not m:
        continue
    s = s[:m.end()] + bar(rel, here) + s[m.end():]
    page.write_text(s, encoding="utf-8")
    n += 1
print(f"site theme: {n} pages")
