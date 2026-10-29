# SPDX-License-Identifier: MPL-2.0
# Copyright (C) 2026 fm1-aether contributors
# AI-generated code (Claude, Anthropic): no warranty, use at your own risk (see NOTICE).
"""The site's installer (fm1-base's web/installer.html, as make_site.py writes it), made quieter:
the per-request log lines kept in a closed "Details" box instead of on the page, the bar sized by this package
(fm1-base counts 1167 write requests, AEther's image takes more, so the bar sat at 100 % while writing went on),
held at 99 % until the end, and moving without a value (the browser's animation) while the FM-1 restarts.
   python3 tools/installer_quiet.py SITE/install/index.html"""
import sys

p = sys.argv[1]
s = open(p, encoding="utf-8").read()


def rep(a, b):
    global s
    if s.count(a) != 1:
        raise SystemExit(f"installer_quiet: fm1-base's installer changed, not found once: {a[:70]!r}")
    s = s.replace(a, b)


rep('<pre id="log"></pre>',
    '<details id="logbox"><summary class="small">Details</summary><pre id="log"></pre></details>')
rep('const log = (t) => { $("log").textContent += t + "\\n"; };',
    'const LOG = [];\n'
    'const log = (t) => { LOG.push(t); if (LOG.length > 300) LOG.splice(0, LOG.length - 300);\n'
    '  if ($("logbox").open) $("log").textContent = LOG.join("\\n"); };\n'
    'document.addEventListener("DOMContentLoaded", () => $("logbox").addEventListener("toggle", () => {\n'
    '  $("log").textContent = LOG.join("\\n"); }));')
rep('const total = 74 + 1167;', 'const total = 74 + Math.max(1167, Math.ceil(img.length / 512));')
rep('log(`${k} ${a ?? ""}`);', 'if (k !== "verify" && k !== "write") log(`${k} ${a ?? ""}`);')
rep('if (k === "write") { $("bar").value = ((74 + a) / total) * 100; sayPct("write", $("bar").value); }',
    'if (k === "write") { const pc = Math.min(99, ((74 + a) / total) * 100); $("bar").value = pc; sayPct("write", pc); }')
rep('if (k === "loader") sayK("loader");', 'if (k === "loader") { $("bar").removeAttribute("value"); sayK("loader"); }')
rep('if (k === "reboot") sayK("reboot");', 'if (k === "reboot") { $("bar").removeAttribute("value"); sayK("reboot"); }')
rep('log(String(e && e.stack || e));', 'log(String(e && e.stack || e)); $("logbox").open = true;')   # a failure: shown
open(p, "w", encoding="utf-8").write(s)
print("installer: quiet log, bar by the package")
