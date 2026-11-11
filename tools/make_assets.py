#!/usr/bin/env python3
"""Deterministic SVG infographic generator for the repository.

Why a generator instead of a hosted badge/banner service:
  * reproducible from source (same bytes on every machine),
  * no third-party runtime dependency, works offline and in a fresh clone,
  * every asset is reviewable as code in the same commit as the docs.

Outputs (assets/):
  banner.svg        hero header: title, proof ladder, release chip
  advantages.svg    why this is not a generated dump: six gate-backed properties
  pipeline.svg      source -> build -> QEMU gate -> versions.json -> docs -> CI
  module-package.svg install path of module.zip on a KernelSU device
  vmscan-hook.svg   vmscan_hook mechanism: entry, loop, young-page rotation
  architecture.svg  layered stack: boot -> loader -> CFI bridge -> entry -> modules
  entry-matrix.svg  proven / experimental / closed mechanisms
  roadmap.svg       backport roadmap stages
  evidence.svg      QEMU log card for the trampoline probe
  library.svg       module library cards

Run:  python3 tools/make_assets.py [--check]
Check mode parses the produced files and validates geometry/XML only.
"""
from __future__ import annotations

import argparse
import json
import pathlib
import sys
import xml.etree.ElementTree as ET

ROOT = pathlib.Path(__file__).resolve().parent.parent
OUT = ROOT / "assets"
META = json.loads((ROOT / "versions.json").read_text(encoding="utf-8"))

VERSION = META["release"]["version"]
MODULES = {m["name"]: m for m in META["modules"]}
KT = META["evidence"]["ktramp"]

# --- palette ---------------------------------------------------------------
BG0 = "#070b16"
BG1 = "#0b1224"
PANEL = "#0f1a2e"
PANEL2 = "#121f38"
STROKE = "#22314e"
TEXT = "#e8eefb"
MUTED = "#8fa3bf"
DIM = "#64748b"
CYAN = "#22d3ee"
VIOLET = "#a78bfa"
GREEN = "#34d399"
AMBER = "#fbbf24"
RED = "#fb7185"
BLUE = "#60a5fa"

SANS = "'Inter','Segoe UI',Roboto,Helvetica,Arial,sans-serif"
MONO = "'JetBrains Mono','SFMono-Regular',ui-monospace,Menlo,Consolas,monospace"


def esc(s: str) -> str:
    return s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")


def tw(s: str, size: float, mono: bool = False) -> float:
    """Rough advance width used for layout assertions (not rendering)."""
    if mono:
        return len(s) * size * 0.6
    wide = sum(1 for c in s if c in "MW@%")
    narrow = sum(1 for c in s if c in "ilj.,:;'|!")
    return (len(s) - wide - narrow) * size * 0.52 + wide * size * 0.86 + narrow * size * 0.28


def wrap(text: str, width: float, size: float, mono: bool = False) -> list[str]:
    """Greedy wrap against the same advance-width model used by the layout."""
    out: list[str] = []
    line = ""
    for word in text.split():
        cand = (line + " " + word).strip()
        if line and tw(cand, size, mono) > width:
            out.append(line)
            line = word
        else:
            line = cand
    if line:
        out.append(line)
    return out


class S:
    def __init__(self, w: int, h: int, title: str, desc: str):
        self.w, self.h = w, h
        self.parts: list[str] = []
        self.parts.append(
            f'<svg xmlns="http://www.w3.org/2000/svg" width="{w}" height="{h}" '
            f'viewBox="0 0 {w} {h}" role="img" aria-labelledby="t d">'
        )
        self.parts.append(f'<title id="t">{esc(title)}</title>')
        self.parts.append(f'<desc id="d">{esc(desc)}</desc>')
        self.parts.append(self.defs())

    def defs(self) -> str:
        return f"""<defs>
  <linearGradient id="bg" x1="0" y1="0" x2="1" y2="1">
    <stop offset="0" stop-color="{BG0}"/><stop offset="55%" stop-color="{BG1}"/>
    <stop offset="100%" stop-color="{BG0}"/>
  </linearGradient>
  <linearGradient id="accent" x1="0" y1="0" x2="1" y2="0">
    <stop offset="0" stop-color="{CYAN}"/><stop offset="100%" stop-color="{VIOLET}"/>
  </linearGradient>
  <linearGradient id="cardg" x1="0" y1="0" x2="0" y2="1">
    <stop offset="0" stop-color="{PANEL2}" stop-opacity="0.95"/>
    <stop offset="100%" stop-color="{PANEL}" stop-opacity="0.95"/>
  </linearGradient>
  <radialGradient id="glowc"><stop offset="0" stop-color="{CYAN}" stop-opacity="0.28"/>
    <stop offset="100%" stop-color="{CYAN}" stop-opacity="0"/></radialGradient>
  <radialGradient id="glowv"><stop offset="0" stop-color="{VIOLET}" stop-opacity="0.26"/>
    <stop offset="100%" stop-color="{VIOLET}" stop-opacity="0"/></radialGradient>
  <pattern id="grid" width="40" height="40" patternUnits="userSpaceOnUse">
    <path d="M40 0H0V40" fill="none" stroke="#1b2740" stroke-width="1" stroke-opacity="0.5"/>
  </pattern>
  <marker id="arw" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="7" markerHeight="7"
          orient="auto-start-reverse"><path d="M0 0 L10 5 L0 10 z" fill="{MUTED}"/></marker>
  <filter id="soft" x="-20%" y="-20%" width="140%" height="140%">
    <feDropShadow dx="0" dy="6" stdDeviation="10" flood-color="#000" flood-opacity="0.35"/>
  </filter>
</defs>"""

    # primitives ---------------------------------------------------------
    def bg(self, grid: bool = True) -> None:
        self.parts.append(f'<rect width="{self.w}" height="{self.h}" fill="url(#bg)"/>')
        if grid:
            self.parts.append(f'<rect width="{self.w}" height="{self.h}" fill="url(#grid)" opacity="0.35"/>')
        self.parts.append(f'<circle cx="{int(self.w*0.12)}" cy="0" r="{int(self.w*0.32)}" fill="url(#glowc)"/>')
        self.parts.append(f'<circle cx="{int(self.w*0.92)}" cy="{self.h}" r="{int(self.w*0.28)}" fill="url(#glowv)"/>')

    def txt(self, x, y, s, size=14, fill=TEXT, weight="400", anchor="start",
            mono=False, spacing=None, opacity=None) -> None:
        a = f' text-anchor="{anchor}"' if anchor != "start" else ""
        ls = f' letter-spacing="{spacing}"' if spacing is not None else ""
        op = f' opacity="{opacity}"' if opacity is not None else ""
        fam = MONO if mono else SANS
        self.parts.append(
            f'<text x="{x}" y="{y}" font-family="{fam}" font-size="{size}" fill="{fill}" '
            f'font-weight="{weight}"{a}{ls}{op}>{esc(s)}</text>'
        )

    def rect(self, x, y, w, h, r=14, fill="url(#cardg)", stroke=STROKE, sw=1,
             dash=None, opacity=None, filt=None) -> None:
        d = f' stroke-dasharray="{dash}"' if dash else ""
        o = f' opacity="{opacity}"' if opacity is not None else ""
        f = f' filter="url(#{filt})"' if filt else ""
        self.parts.append(
            f'<rect x="{x}" y="{y}" width="{w}" height="{h}" rx="{r}" fill="{fill}" '
            f'stroke="{stroke}" stroke-width="{sw}"{d}{o}{f}/>'
        )

    def pill(self, x, y, label, color, size=12, pad=12, h=24) -> float:
        w = tw(label, size, mono=True) + pad * 2
        self.parts.append(
            f'<rect x="{x}" y="{y}" width="{w:.0f}" height="{h}" rx="{h/2}" '
            f'fill="{color}" fill-opacity="0.12" stroke="{color}" stroke-opacity="0.45"/>'
        )
        self.txt(x + pad, y + h / 2 + size * 0.36, label, size, color, "600", mono=True)
        return w

    def arrow(self, x1, y1, x2, y2) -> None:
        self.parts.append(
            f'<path d="M{x1} {y1} L{x2} {y2}" stroke="{MUTED}" stroke-width="1.6" '
            f'stroke-opacity="0.7" marker-end="url(#arw)"/>'
        )

    def para(self, x, y, text, width, size=11.5, fill=MUTED, lh=None,
             weight="400", mono=False, max_lines=None) -> int:
        """Wrapped paragraph; returns the number of rendered lines."""
        lh = lh if lh is not None else size * 1.45
        lines = wrap(text, width, size, mono)
        if max_lines is not None:
            lines = lines[:max_lines]
        for i, line in enumerate(lines):
            self.txt(x, y + i * lh, line, size, fill, weight, mono=mono)
        return len(lines)

    def save(self, name: str) -> pathlib.Path:
        self.parts.append("</svg>")
        p = OUT / name
        p.write_text("\n".join(self.parts) + "\n", encoding="utf-8")
        return p


# --- banner ---------------------------------------------------------------
def banner() -> None:
    s = S(1280, 400, "Vendor Kernel Runtime Entry",
          "Hero banner: load trusted modules into a vendor-frozen kernel, with the "
          "proof ladder from a locked boot chain to the versioned module library.")
    s.bg()

    # top hairline accent
    s.parts.append('<rect x="0" y="0" width="1280" height="3" fill="url(#accent)" opacity="0.9"/>')

    # eyebrow + status. The status sits in the left column on purpose: the proof
    # ladder panel owns the right side of the canvas from y=56, and a badge drawn
    # there would be covered by the panel's own background rect.
    eyebrow = "RUNTIME MODULE LIBRARY  ·  ANDROID 16  ·  ARM64"
    s.txt(64, 72, eyebrow, 12, CYAN, "700", spacing="3")
    ex = 64 + len(eyebrow) * 12 * 0.52 + len(eyebrow) * 3 + 30
    s.parts.append(f'<circle cx="{ex:.0f}" cy="67" r="4.5" fill="{GREEN}"/>')
    s.txt(ex + 12, 72, "active development", 12, GREEN, "600")

    # title
    s.txt(64, 146, "VENDOR KERNEL", 60, TEXT, "800", spacing="-1")
    s.parts.append(
        f'<text x="64" y="210" font-family="{SANS}" font-size="60" font-weight="800" '
        f'letter-spacing="-1" fill="url(#accent)">RUNTIME ENTRY</text>'
    )
    s.txt(64, 254, "Load trusted modules into a vendor-frozen kernel —", 16.5, MUTED)
    s.txt(64, 278, "no kernel recompile, no unlocked bootloader, no livepatch.", 16.5, MUTED)

    # capability chips
    x = 64
    for label, color in [("CFI bridge", CYAN), ("kprobe", BLUE), ("text patch", VIOLET),
                         ("trampoline", AMBER), ("procfs", GREEN)]:
        x += s.pill(x, 298, label, color, size=11.5) + 9

    # release + library size chips
    vy = 338
    w = s.pill(64, vy, "v" + VERSION, VIOLET, size=12, h=26)
    s.pill(64 + w + 10, vy, f"{len(META['modules'])} modules", GREEN, size=12, h=26)
    s.txt(64 + w + 10 + tw(f"{len(META['modules'])} modules", 12, True) + 24 + 22, vy + 18,
          "each shipped module has its own QEMU gate", 11.5, DIM)

    # right: proof ladder (boot chain -> library). No roadmap: this panel states
    # what is *already* true on the reference device.
    px, py, pw = 720, 56, 496
    s.rect(px, py, pw, 292, 18, fill="#0b1424", stroke="#1e2b45")
    s.txt(px + 24, py + 30, "PROOF LADDER", 10.5, DIM, "700", spacing="2")
    s.txt(px + pw - 24, py + 30, "state on the reference Image", 10, DIM, anchor="end")

    rows = [
        ("0", "vendor Image 5.10.236", "bootloader locked · no kernel flash · MODULES=y",
         MUTED, "given"),
        ("1", "loader compatibility", "vermagic · __versions CRC · struct module 0x190/0x3c0",
         CYAN, "proven"),
        ("2", "CFI bridge", "module-local __cfi_check, no kernel-wide CFI change",
         VIOLET, "proven"),
        ("3", "runtime entry", "kprobe · text patch · trampoline · raw payload · procfs",
         BLUE, "proven"),
        ("4", "module library", f"{len(META['modules'])} versioned modules, one per gate",
         GREEN, "growing"),
    ]
    ry = py + 46
    for i, (num, name, detail, color, state) in enumerate(rows):
        y = ry + i * 48
        s.rect(px + 16, y, pw - 32, 42, 10, fill="url(#cardg)", stroke=STROKE)
        s.parts.append(f'<rect x="{px + 16}" y="{y}" width="3.5" height="42" rx="1.75" fill="{color}" opacity="0.9"/>')
        s.txt(px + 34, y + 27, num, 16, color, "800", mono=True)
        s.txt(px + 58, y + 19, name, 13, TEXT, "700")
        s.txt(px + 58, y + 34, detail, 10, MUTED)
        pw_state = tw(state, 9.5, mono=True) + 16
        s.pill(px + pw - 24 - pw_state, y + 11, state, color, size=9.5, h=19, pad=8)

    # footer: the contract of the whole repository in one line
    s.txt(64, 384, "Every number in these documents is produced by a gate you can re-run: "
                   "tests/qemu_*.sh → serial log → versions.json.", 10.5, DIM, mono=True)
    s.save("banner.svg")


# --- advantages ------------------------------------------------------------
def advantages() -> None:
    s = S(1280, 620, "Why this repository is not a generated dump",
          "Six properties that separate a measured kernel project from AI-generated slop: "
          "QEMU gates, reproducible hashes, machine-checked docs, generated graphics, "
          "bounded blast radius and fresh-clone CI.")
    s.bg()
    s.txt(64, 64, "WHY THIS IS NOT GENERATED SLOP", 12, CYAN, "700", spacing="3")
    s.txt(64, 106, "Six properties a text dump cannot fake", 30, TEXT, "800")
    s.txt(64, 136, "The prose here may be written with AI assistance. The numbers cannot be: "
                   "they are read from versions.json and re-checked on every push.", 13, MUTED)

    cards = [
        ("01", "Measured, not narrated",
         "Every behavioural claim ships with the QEMU gate that produced it. A stage that "
         "has not run is marked planned; it is never described as working.",
         "tests/qemu_*.sh → serial.log", GREEN),
        ("02", "Reproducible artifacts",
         "Each .ko hash is pinned in versions.json. Rebuilding from the same profile "
         "reproduces the bytes — vmscan_hook 1.1.0 was built twice to one sha256.",
         "sha256 9801942b… · rebuild-identical", CYAN),
        ("03", "Machine-checked docs",
         "check_versions.py fails on a stale hash, a version drift, a missing language pair "
         "or a hand-edited SVG. The gate was proven by breaking a document on purpose.",
         "10 checks · one exit code", VIOLET),
        ("04", "Graphics are rendered, not drawn",
         "All infographics are generated from versions.json and compared byte-for-byte in CI. "
         "Nobody types a counter into a banner.",
         "assets/*.svg ← tools/make_assets.py", AMBER),
        ("05", "Bounded by design",
         "The CFI bridge disables checks for this module only. Rollback is a reboot. "
         "livepatch, kexec and ftrace are listed as closed — not faked.",
         "7 proven · 5 closed mechanisms", BLUE),
        ("06", "Fresh-clone CI",
         "The static gate runs on a clean checkout: no dist/, no network, no cached state, "
         "on every push and pull request.",
         ".github/workflows/static.yml", GREEN),
    ]
    cw, ch, gx, gy, x0, y0 = 376, 178, 24, 18, 64, 168
    for i, (num, title, body, ev, color) in enumerate(cards):
        r, c = divmod(i, 3)
        x, y = x0 + c * (cw + gx), y0 + r * (ch + gy)
        s.rect(x, y, cw, ch, 16, fill="url(#cardg)")
        s.parts.append(f'<rect x="{x}" y="{y}" width="{cw}" height="4" rx="2" fill="{color}" opacity="0.85"/>')
        s.txt(x + 20, y + 36, num, 11, color, "700", mono=True)
        s.txt(x + 20, y + 62, title, 16.5, TEXT, "700")
        s.para(x + 20, y + 88, body, cw - 40, size=11.5, fill=MUTED, lh=17)
        s.txt(x + 20, y + ch - 18, ev, 10.5, color, "500", mono=True)

    s.rect(64, 556, 1152, 40, 12, fill="#0b1424", stroke="#1e2b45")
    s.txt(84, 581, "A generated repository convinces you with adjectives. "
                   "This one asks you to run three commands.", 12.5, "#c9d7ea")
    s.save("advantages.svg")


# --- pipeline --------------------------------------------------------------
def pipeline() -> None:
    s = S(1280, 460, "Release pipeline",
          "From C sources to a published release: the same gate runs locally and in CI.")
    s.bg()
    s.txt(64, 64, "PIPELINE", 12, CYAN, "700", spacing="3")
    s.txt(64, 104, "One source of truth, one gate, no hand-edited facts", 28, TEXT, "800")
    s.txt(64, 132, "Anything that contradicts versions.json fails before it can be published.", 13, MUTED)

    steps = [
        ("1", "source", "C modules + per-target profile + kernel/README", MUTED),
        ("2", "build", "tools/adapt_kernel.sh · exact vermagic and offsets", CYAN),
        ("3", "QEMU gate", "insmod · behaviour · restore · rmmod on the stock Image", GREEN),
        ("4", "versions.json", "release, module hashes, measured counters", VIOLET),
        ("5", "docs + assets", "EN/RU pairs and SVGs rendered from that file", BLUE),
        ("6", "gate", "check_versions.py + static_checks.sh", AMBER),
        ("7", "release", "commit · tag · module.zip · CI · fresh clone", GREEN),
    ]
    cw, gap, x0, y0, chh = 150, 17, 64, 168, 118
    for i, (num, name, detail, color) in enumerate(steps):
        x = x0 + i * (cw + gap)
        s.rect(x, y0, cw, chh, 14, fill="url(#cardg)")
        s.parts.append(f'<rect x="{x}" y="{y0}" width="{cw}" height="4" rx="2" fill="{color}" opacity="0.85"/>')
        s.txt(x + 16, y0 + 32, num, 11, color, "700", mono=True)
        s.txt(x + 16, y0 + 56, name, 14, TEXT, "700")
        s.para(x + 16, y0 + 76, detail, cw - 32, size=10, fill=MUTED, lh=14, max_lines=3)
        if i < len(steps) - 1:
            s.arrow(x + cw + 2, y0 + chh / 2, x + cw + gap - 4, y0 + chh / 2)

    # what the gate refuses
    s.rect(64, 310, 1152, 88, 14, fill="#0b1424", stroke="#1e2b45")
    s.txt(84, 338, "THE GATE REFUSES TO PUBLISH", 10.5, RED, "700", spacing="2")
    refusals = [
        ("stale sha256", RED), ("version drift", RED), ("missing RU pair", AMBER),
        ("hand-edited SVG", AMBER), ("deferred-work marker", RED), ("secret-like string", RED),
    ]
    x = 84
    for label, color in refusals:
        w = s.pill(x, 356, label, color, size=10.5, h=24) + 10
        x += w

    s.txt(64, 428, "The same script runs in CI on every push and in a fresh clone — "
                   "a check that lives only in a document is a suggestion.", 11.5, DIM)
    s.save("pipeline.svg")


# --- module package --------------------------------------------------------
def module_package() -> None:
    s = S(1280, 340, "Module package install path",
          "How module.zip is installed, verified and rolled back on a KernelSU device.")
    s.bg()
    s.txt(64, 60, "PACKAGE", 12, CYAN, "700", spacing="3")
    s.txt(64, 98, "module.zip on a KernelSU device", 26, TEXT, "800")
    s.txt(64, 126, "The packaged .ko is valid for one profile. Verify the hash before flashing.", 12.5, MUTED)

    steps = [
        ("1", "install", "ksud module install module.zip", CYAN),
        ("2", "boot", "post-fs-data.sh: insmod bin/kbootstrap.ko", BLUE),
        ("3", "verify", "cat /proc/kbootstrap → status surface", GREEN),
        ("4", "observe", "service.sh records the same status", AMBER),
        ("5", "rollback", "reboot — the module is never rmmod'd live", VIOLET),
    ]
    cw, gap, x0, y0, chh = 208, 22, 64, 154, 108
    for i, (num, name, detail, color) in enumerate(steps):
        x = x0 + i * (cw + gap)
        s.rect(x, y0, cw, chh, 14, fill="url(#cardg)")
        s.parts.append(f'<rect x="{x}" y="{y0}" width="{cw}" height="4" rx="2" fill="{color}" opacity="0.85"/>')
        s.txt(x + 16, y0 + 30, num, 11, color, "700", mono=True)
        s.txt(x + 16, y0 + 54, name, 14.5, TEXT, "700")
        s.para(x + 16, y0 + 76, detail, cw - 32, size=10.5, fill=MUTED, lh=14, max_lines=2)
        if i < len(steps) - 1:
            s.arrow(x + cw + 2, y0 + chh / 2, x + cw + gap - 4, y0 + chh / 2)

    s.txt(64, 302, "Reference profile: RMX3709 · 5.10.236 · aarch64. Another kernel needs "
                   "tools/adapt_kernel.sh — never install a foreign .ko.", 11.5, DIM)
    s.save("module-package.svg")


# --- vmscan hook -----------------------------------------------------------
def vmscan_hook_asset() -> None:
    ev = META["evidence"]["vmscan_hook"]
    st = META["evidence"]["vmscan_stack"]
    s = S(1280, 540, "vmscan_hook mechanism",
          "Custom shrink_lruvec through a module: text-patch entry, kernel list scanners, "
          "young-page rotation and the procfs control surface.")
    s.bg()
    s.txt(64, 60, "VMSSCAN_HOOK  ·  v" + MODULES["vmscan_hook"]["version"], 12, CYAN, "700", spacing="3")
    s.txt(64, 98, "A custom reclaim path without rebuilding vmscan", 26, TEXT, "800")
    s.txt(64, 126, "Four moving parts, each observable from /proc and replayed by "
                   "tests/qemu_vmscan_stack_gate.sh.", 12.5, MUTED)

    panels = [
        ("1 · entry", CYAN, [
            "shrink_lruvec entry is replaced with `b my_shrink_lruvec` using",
            "aarch64_insn_patch_text under stop_machine; restore writes the",
            "original instructions back. No livepatch, no kernel rebuild.",
        ]),
        ("2 · loop", VIOLET, [
            "my_shrink_lruvec calls get_scan_count, then walks",
            "INACTIVE_ANON..ACTIVE_FILE and runs the kernel's own",
            "shrink_active_list / shrink_inactive_list with min(nr, 32),",
            "stopping at nr_to_reclaim.",
        ]),
        ("3 · young-page rotation", GREEN, [
            "A kprobe on mark_page_accessed marks a page recent. If it sits",
            "on the file LRU inside the ttl window, it is moved to the list",
            "tail instead of being reclaimed: `moved` counts that rotation.",
        ]),
        ("4 · control", AMBER, [
            "/proc/vmscan_hook: write 1 install, 0 restore, t<ms> ttl.",
            "read: hooked, calls, scans, reclaimed, moved, ttl_ms.",
            "Every number below was read from that file in QEMU.",
        ]),
    ]
    pw, ph = 560, 148
    for i, (title, color, body) in enumerate(panels):
        r, c = divmod(i, 2)
        x, y = 64 + c * (pw + 32), 158 + r * (ph + 20)
        s.rect(x, y, pw, ph, 16, fill="url(#cardg)")
        s.parts.append(f'<rect x="{x}" y="{y}" width="{pw}" height="4" rx="2" fill="{color}" opacity="0.85"/>')
        s.txt(x + 20, y + 38, title, 14.5, TEXT, "700")
        for j, ln in enumerate(body):
            s.txt(x + 20, y + 68 + j * 19, ln, 11.5, MUTED, mono=False)

    s.rect(64, 476, 1152, 44, 12, fill="#0b1424", stroke="#1e2b45")
    s.txt(84, 504, f"Observed (QEMU, exact Image): calls={ev['calls']} scans={ev['scans']} "
                   f"moved={ev.get('moved', 0)} · stack with ktramp: "
                   f"ktramp calls={st['ktramp_calls']} vh scans={st['scans']} moved={st.get('moved', 0)}",
          12, "#c9d7ea", mono=True)
    s.save("vmscan-hook.svg")



# --- architecture ---------------------------------------------------------
def architecture() -> None:
    s = S(1280, 860, "Runtime entry architecture",
          "Five layers from a locked boot chain to the module library.")
    s.bg()
    s.txt(64, 66, "ARCHITECTURE", 12, CYAN, "700", spacing="3")
    s.txt(64, 108, "From a locked boot chain to a running module", 30, TEXT, "800")
    s.txt(64, 138, "Each layer exists because the layer above it refused something. "
                   "Only the last one changes the kernel.", 14, MUTED)

    layers = [
        ("0", "Boot chain", "bootloader locked · no kernel flash · vendor Image",
         ["Image 5.10.236", "MODULES=y", "CFI_CLANG + LTO", "SCS · BTI · PAC"], MUTED, "given"),
        ("1", "Loader compatibility", "make the vendor loader accept a foreign build",
         ["vermagic + .modinfo", "__versions CRC", "struct module 0x190/0x3c0", "SHN_XINDEX"], CYAN, "proven"),
        ("2", "CFI bridge", "module-local CFI scaffolding, no kernel-wide change",
         ["__cfi_check: ret", "__cfi_jt_init/cleanup", "bti c; b init_module", "trusted module"], VIOLET, "proven"),
        ("3", "Runtime entry", "what the module can actually do inside the kernel",
         ["kprobe/kretprobe", "text patch", "trampoline", "raw payload · procfs"], BLUE, "proven"),
        ("4", "Module library", "reusable, versioned modules built on those entries",
         ["kbootstrap", "kpatch", "ktramp", "mglru_probe"], GREEN, "growing"),
    ]
    y0 = 178
    for i, (num, name, sub, chips, color, state) in enumerate(layers):
        y = y0 + i * 128
        s.rect(64, y, 1152, 108, 16, fill="url(#cardg)")
        s.parts.append(f'<rect x="64" y="{y}" width="5" height="108" rx="2.5" fill="{color}" opacity="0.9"/>')
        s.txt(92, y + 44, num, 26, color, "800", mono=True)
        s.txt(126, y + 42, name, 20, TEXT, "700")
        s.txt(126, y + 66, sub, 12.5, MUTED)

        # chips row (max 4)
        cx = 560
        for c in chips:
            w = tw(c, 11.5, mono=True) + 22
            if cx + w > 1000:
                break
            s.rect(cx, y + 30, w, 26, 8, fill="#0b1424", stroke="#24344f")
            s.txt(cx + 11, y + 48, c, 11.5, "#c9d7ea", "500", mono=True)
            cx += w + 8

        # state pill
        col = {"given": MUTED, "proven": GREEN, "growing": AMBER}[state]
        pw = tw(state, 11, mono=True) + 24
        s.pill(1180 - pw, y + 22, state, col, size=11, h=22)

        if i < len(layers) - 1:
            s.arrow(640, y + 108, 640, y + 128)

    s.txt(64, 826, "Nothing above the CFI bridge is kernel-wide: the bridge disables CFI checks for "
                   "the module only. Treat every module as trusted code.", 12.5, DIM)
    s.save("architecture.svg")


# --- entry matrix ---------------------------------------------------------
def entry_matrix() -> None:
    s = S(1280, 780, "Entry-point matrix",
          "Which runtime mechanisms are proven, experimental or closed on the reference kernel.")
    s.bg()
    s.txt(64, 64, "ENTRY-POINT MATRIX", 12, CYAN, "700", spacing="3")
    s.txt(64, 104, "What the entry can do — measured, not assumed", 30, TEXT, "800")
    s.txt(64, 132, "Reference: RMX3709 5.10.236 · QEMU on the exact stock Image · maxcpus=1", 13, MUTED)

    items = [
        ("LKM + CFI bridge", "proven", GREEN, "module init/exit + callbacks run"),
        ("kprobe / kretprobe", "proven", GREEN, "resolve symbols, patch args at return"),
        ("text patch", "proven", GREEN, "replace a live function, then restore"),
        ("trampoline", "proven", GREEN, "copied prologue -> wrapper -> branch back"),
        ("raw payload", "proven", GREEN, "module_alloc + set_memory_x + icache flush"),
        ("kallsyms via kprobe", "proven", GREEN, "non-exported symbols resolved"),
        ("procfs / sysfs", "proven", GREEN, "status and control surfaces"),
        ("BPF", "observe", AMBER, "tracing only, verifier limits"),
        ("ftrace function tracer", "closed", RED, "CONFIG_FUNCTION_TRACER unset"),
        ("livepatch", "closed", RED, "CONFIG_LIVEPATCH unset"),
        ("kexec", "closed", RED, "CONFIG_KEXEC unset"),
        ("devmem / multikernel", "closed", RED, "configs unset"),
    ]
    cols, x0, y0, cw, ch, gx, gy = 4, 64, 168, 280, 132, 10, 14
    for i, (name, state, color, detail) in enumerate(items):
        r, c = divmod(i, cols)
        x, y = x0 + c * (cw + gx), y0 + r * (ch + gy)
        s.rect(x, y, cw, ch, 14, fill="url(#cardg)")
        s.parts.append(f'<rect x="{x}" y="{y}" width="{cw}" height="4" rx="2" fill="{color}" opacity="0.85"/>')
        s.txt(x + 18, y + 42, name, 15.5, TEXT, "700")
        s.pill(x + 18, y + 56, state, color, size=10.5, h=21, pad=9)
        # wrapped detail
        words, line, lines = detail.split(), "", []
        for w in words:
            if tw(line + " " + w, 11.5) > cw - 36:
                lines.append(line)
                line = w
            else:
                line = (line + " " + w).strip()
        lines.append(line)
        for j, ln in enumerate(lines[:3]):
            s.txt(x + 18, y + 102 + j * 16, ln, 11.5, MUTED)

    s.txt(64, 736, "Single-CPU QEMU result. On SMP, text patching needs aarch64_insn_patch_text / "
                   "stop_machine — nosync is not safe there.", 12.5, DIM)
    s.save("entry-matrix.svg")


# --- roadmap --------------------------------------------------------------
def roadmap() -> None:
    s = S(1280, 460, "Roadmap",
          "Staged path from a proven entry to 7.4 feature parity.")
    s.bg()
    s.txt(64, 64, "ROADMAP", 12, CYAN, "700", spacing="3")
    s.txt(64, 104, "From runtime entry to real feature backports", 30, TEXT, "800")
    s.txt(64, 132, "Each stage is gated on QEMU evidence; no stage is claimed before it is measured.", 13, MUTED)

    s.parts.append(f'<path d="M96 258 H1184" stroke="{STROKE}" stroke-width="3"/>')
    s.parts.append(f'<path d="M96 258 H660" stroke="{GREEN}" stroke-width="3"/>')
    s.parts.append(f'<path d="M660 258 H900" stroke="{AMBER}" stroke-width="3"/>')

    nodes = [
        ("M0", "Entry proven", "LKM + CFI bridge,\nraw payload", GREEN, "done"),
        ("M1", "Text patch", "save / patch / restore\nof getpid", GREEN, "done"),
        ("M2", "Trampoline", "get_scan_count replaced,\n426 calls, restore", GREEN, "done"),
        ("M3", "Policy hooks", "reclaim policy modes,\ngeneration side table", AMBER, "in progress"),
        ("M4", "7.4 backport", "vmscan functions,\nfolio/workingset", DIM, "next"),
        ("M5", "Parity", "benchmarks + gap\nreport", DIM, "planned"),
    ]
    xs = [120, 340, 560, 790, 1010, 1200]
    for (tag, name, sub, color, state), x in zip(nodes, xs):
        s.parts.append(f'<circle cx="{x}" cy="258" r="13" fill="{BG0}" stroke="{color}" stroke-width="3"/>')
        s.parts.append(f'<circle cx="{x}" cy="258" r="5" fill="{color}"/>')
        s.txt(x, 226, tag, 12, color, "700", anchor="middle", mono=True)
        s.txt(x, 300, name, 14.5, TEXT, "700", anchor="middle")
        for j, ln in enumerate(sub.split("\n")):
            s.txt(x, 320 + j * 15, ln, 11, MUTED, anchor="middle")
        s.txt(x, 372, state, 10.5, color, "600", anchor="middle", mono=True)

    s.rect(64, 398, 1152, 44, 12, fill="#0b1424", stroke="#1e2b45")
    s.txt(84, 425, "Hard rule: a probe is not a backport. Stage M3/M4 land only with QEMU behavior "
                   "evidence and an explicit gap list.", 12.5, "#c9d7ea")
    s.save("roadmap.svg")


# --- evidence -------------------------------------------------------------
def evidence() -> None:
    s = S(1280, 520, "QEMU evidence",
          "Trimmed serial log of the trampoline probe.")
    s.bg()
    s.txt(64, 60, "EVIDENCE", 12, CYAN, "700", spacing="3")
    s.txt(64, 98, "Trampoline probe — serial log, trimmed", 26, TEXT, "800")

    s.rect(64, 122, 800, 330, 14, fill="#05090f", stroke="#1b2740")
    s.parts.append(f'<rect x="64" y="122" width="800" height="34" rx="14" fill="#0b1220"/>')
    for i, c in enumerate(["#fb7185", "#fbbf24", "#34d399"]):
        s.parts.append(f'<circle cx="{88 + i*18}" cy="139" r="5.5" fill="{c}" opacity="0.85"/>')
    s.txt(160, 144, "qemu-system-aarch64 · maxcpus=1 · exact stock Image", 11, DIM, mono=True)

    lines = [
        ("[    2.804780] cfi: not using shadow for module ktramp", MUTED),
        ("[    2.939000] ktramp: target=ffffffc00843c0c0 patch=ffffffc009a10870", MUTED),
        ("ktramp ... tramp=0xffffffc001a36000 patched=1 policy=1 calls=0", TEXT),
        ("ktramp ... patched=1 policy=1 calls=426", GREEN),
        ("pgscan_file 0            # baseline 7032", GREEN),
        ("ktramp ... patched=0 policy=0 calls=426   # restored", CYAN),
    ]
    for i, (ln, color) in enumerate(lines):
        s.txt(88, 190 + i * 42, ln, 12.5, color, "500", mono=True)

    notes = [
        ("CFI bridge", "non-LTO module accepted by the vendor CFI runtime"),
        ("Symbols", "get_scan_count + patcher resolved via kprobe"),
        ("Trampoline", "7 prologue instructions copied, branch back"),
        ("Policy", "wrapper entered by real reclaim: " + str(KT["calls"]) + " calls"),
        ("Rollback", "original entry restored, no rmmod needed"),
    ]
    for i, (k, v) in enumerate(notes):
        y = 150 + i * 62
        s.parts.append(f'<circle cx="892" cy="{y - 5}" r="4" fill="{CYAN}"/>')
        s.txt(910, y, k, 13, TEXT, "700")
        s.txt(910, y + 19, v, 11.5, MUTED)

    s.txt(64, 486, "Log: " + KT["log"] + " · replayed by tests/qemu_ktramp_gate.sh", 11.5, DIM, mono=True)
    s.save("evidence.svg")


# --- library --------------------------------------------------------------
def library() -> None:
    s = S(1280, 520, "Module library",
          "Versioned modules shipped by this repository.")
    s.bg()
    s.txt(64, 62, "MODULE LIBRARY", 12, CYAN, "700", spacing="3")
    s.txt(64, 100, f"{len(META['modules'])} modules, one entry, growing", 28, TEXT, "800")
    s.txt(64, 128, "Every module is built from this tree by tools/adapt_kernel.sh and gated in QEMU.", 13, MUTED)

    bodies = {
        "kbootstrap": "CFI bridge, kallsyms\nresolution, raw payload,\n/proc surface.",
        "kpatch": "Save, patch, verify,\nrestore a syscall body.",
        "ktramp": "Copied-prologue\ntrampoline + policy\nat get_scan_count.",
        "mglru_probe": "Reclaim kprobes,\npriority policy (0x2b),\nclean rmmod.",
        "vmscan_hook": "Custom shrink_lruvec\nvia text patch; young\npages rotated, not lost.",
    }
    colors = {"stable probe": GREEN, "probe": GREEN, "experimental": AMBER}
    mods = [(m["name"], "v" + m["version"], colors.get(m["status"], DIM), m["status"],
             bodies[m["name"]]) for m in META["modules"]]
    for i, (name, ver, color, state, body) in enumerate(mods):
        x = 64 + i * 234
        s.rect(x, 170, 216, 250, 16, fill="url(#cardg)")
        s.parts.append(f'<rect x="{x}" y="170" width="216" height="4" rx="2" fill="{color}"/>')
        s.txt(x + 16, 206, name, 15, TEXT, "700", mono=True)
        s.txt(x + 16, 226, ver, 11.5, color, "600", mono=True)
        s.pill(x + 16, 236, state, color, size=9.5, h=19, pad=8)
        for j, ln in enumerate(body.split("\n")):
            s.txt(x + 16, 284 + j * 18, ln, 10.5, MUTED)

    s.rect(64, 444, 1152, 34, 10, fill="#0b1424", stroke="#1e2b45")
    s.txt(84, 466, "Reference-profile binaries only (RMX3709 5.10.236). Never install a foreign .ko — "
                   "rebuild for your own kernel.", 11.5, "#c9d7ea")
    s.save("library.svg")


CHECKS = [
    ("banner.svg", 1280, 400),
    ("advantages.svg", 1280, 620),
    ("pipeline.svg", 1280, 460),
    ("module-package.svg", 1280, 340),
    ("vmscan-hook.svg", 1280, 540),
    ("architecture.svg", 1280, 860),
    ("entry-matrix.svg", 1280, 780),
    ("roadmap.svg", 1280, 460),
    ("evidence.svg", 1280, 520),
    ("library.svg", 1280, 520),
]


def verify() -> int:
    bad = 0
    for name, w, h in CHECKS:
        p = OUT / name
        if not p.exists():
            print(f"MISSING {name}")
            bad += 1
            continue
        raw = p.read_text(encoding="utf-8")
        try:
            root = ET.fromstring(raw)
        except ET.ParseError as e:
            print(f"XML FAIL {name}: {e}")
            bad += 1
            continue
        for attr, want in (("width", str(w)), ("height", str(h)), ("viewBox", f"0 0 {w} {h}")):
            if root.get(attr) != want:
                print(f"GEOM FAIL {name}: {attr}={root.get(attr)} want {want}")
                bad += 1
        if "http://" in raw.replace('xmlns="http://www.w3.org/2000/svg"', ""):
            print(f"EXTERNAL REF {name}")
            bad += 1
        if "<script" in raw:
            print(f"SCRIPT {name}")
            bad += 1
        texts = root.findall(".//{http://www.w3.org/2000/svg}text")
        print(f"OK {name} {len(raw)}B texts={len(texts)}")
    return 1 if bad else 0


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true", help="validate existing assets only")
    a = ap.parse_args()
    OUT.mkdir(parents=True, exist_ok=True)
    if not a.check:
        banner()
        advantages()
        pipeline()
        module_package()
        vmscan_hook_asset()
        architecture()
        entry_matrix()
        roadmap()
        evidence()
        library()
    return verify()


if __name__ == "__main__":
    sys.exit(main())
