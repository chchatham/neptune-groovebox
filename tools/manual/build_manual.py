#!/usr/bin/env python3
"""Builds docs/Neptune-Groovebox-Manual.pdf.

  tools/manual/dump_params.sh /tmp/params.json            # parameter pages, read from the real classes
  NEPTUNE_DUMP_SCREENS=/tmp/screens <the app binary>      # every screen and page as a PBM
  python3 tools/manual/build_manual.py /tmp/params.json /tmp/screens docs/Neptune-Groovebox-Manual.pdf

Prose is in chapters.py, short-name descriptions in glossary_data.py. A label in the dump with no
description stops the build, so the appendix cannot fall behind the code. Rendering uses headless
Chrome (two passes: the first finds the page of every heading for the contents list).
"""
import html, json, os, re, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from glossary_data import DESC, EXTRA
import chapters

CHROME = "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome"

ENGINE_NAMES = {"Unified Voice": "Synth", "FM Synth": "FM", "Macro Oscillator": "Macro", "Wavetable": "Wavetable",
                "Wave Sequencer": "Wave seq", "Sampler": "Sampler", "Drum": "Drums", "MIDI Track": "MIDI track"}
FX_NAMES = {"CHR": "Chorus", "DRV": "Drive", "FLG": "Flanger", "PHS": "Phaser", "TRM": "Tremolo", "DLY": "Delay",
            "RVB": "Reverb", "EQ": "Three-band EQ", "CMP": "Compressor", "LMT": "Limiter", "FLT": "Filter",
            "ENS": "Ensemble", "PAN": "Auto-pan", "ROT": "Rotary speaker", "WSH": "Waveshaper", "RED": "Rate reducer",
            "HALL": "Hall reverb", "SPRG": "Spring reverb", "SHIM": "Shimmer reverb", "RVSE": "Reverse delay",
            "TRNS": "Transient shaper", "RING": "Ring modulator", "VIB": "Vibrato", "WAH": "Wah", "AMP": "Amp simulator",
            "GEQ": "Four-band graphic EQ", "TAPE": "Tape delay", "LCR": "Left-centre-right delay",
            "MTAP": "Multi-tap delay", "DUCK": "Sidechain compressor"}
MACRO_MODELS = [("VA", "virtual analog", "SAWS SPRD HARM MORP"), ("FM", "two-operator FM", "INDX RATI DETN FDBK"),
                ("FOLD", "wavefolder", "FOLD SHPE BIAS CHEB"), ("FORM", "formant", "VOWL SHFT Q MORP"),
                ("HARM", "harmonic", "TILT EVOD NUM STRC"), ("PLCK", "plucked string", "DAMP PICK FDBK MORP"),
                ("DUST", "particle noise", "DENS COLR RING MIX"), ("BOW", "bowed string", "PRES POS DAMP SPD")]


def esc(s):
    return html.escape(str(s))


# ---------------------------------------------------------------------------------------------
# Screens: PBM -> inline SVG
# ---------------------------------------------------------------------------------------------
class Screens:
    def __init__(self, folder):
        self.files = {}
        for f in sorted(os.listdir(folder)):
            m = re.match(r"\d+ (.*) (\d+) of (\d+)\.pbm$", f)
            if m:
                self.files[(m.group(1), int(m.group(2)))] = os.path.join(folder, f)

    def svg(self, name, page=1, caption=None, cls=""):
        path = self.files.get((name, page))
        if path is None:
            raise SystemExit(f"no screen bitmap for {name} page {page}")
        vals = open(path).read().split()[3:]
        d = []
        for y in range(64):
            x = 0
            row = vals[y * 128:(y + 1) * 128]
            while x < 128:
                if row[x] == "1":
                    x0 = x
                    while x < 128 and row[x] == "1":
                        x += 1
                    d.append(f"M{x0} {y}h{x - x0}v1h-{x - x0}z")
                else:
                    x += 1
        cap = f"<figcaption>{caption}</figcaption>" if caption else ""
        return (f'<figure class="oled {cls}"><svg viewBox="-3 -3 134 70" role="img"><rect x="-3" y="-3" width="134" '
                f'height="70" rx="3" fill="#0b0d10"/><path d="{"".join(d)}" fill="#bfe9ff"/></svg>{cap}</figure>')


# ---------------------------------------------------------------------------------------------
# Glossary
# ---------------------------------------------------------------------------------------------
def norm_label(label, device, page):
    if device == "FM" and page in ("FMMIX",) and re.fullmatch(r"LVL\d", label): return "LVL#", "LVL1–LVL6"
    if device == "FM" and re.fullmatch(r"RAT\d", label): return "RAT#", "RAT1–RAT6"
    if device == "Wave seq":
        m = re.fullmatch(r"([PSLT])\d+", label)
        if m: return m.group(1) + "#", f"{m.group(1)}1–{m.group(1)}16"
    if re.fullmatch(r"CC\d+", label): return "CC#", "CC16–CC23"
    return label, label


def norm_page(page):
    if re.fullmatch(r"OP\d", page): return "OP1–OP6"
    if re.fullmatch(r"DR \d+", page): return "DR"
    if re.fullmatch(r"LFO[12]", page): return "LFO1 / LFO2"
    m = re.fullmatch(r"(PIT|SHP|LVL|TIM) [AB]", page)
    if m: return f"{m.group(1)} A / B"
    return page


def num(v):
    return ("%g" % round(v, 3)).replace("-", "−")


def values_text(s):
    if s["choices"]:
        ch = s["choices"]
        if len(ch) <= 9 and sum(len(c) for c in ch) <= 44:
            return " ".join(ch)
        return f"{len(ch)} values (App. C)"
    lo, hi, u = s["min"], s["max"], s["unit"]
    if u == "cc": return "0–127"
    if u == "" and s["kind"] in (0, 1):
        if lo == 0 and hi == 1: return "0–100"
        if lo == -1 and hi == 1: return "−100…+100"
        return f"{num(lo * 100)}–{num(hi * 100)}"
    if u == "Hz" and hi >= 1000:
        return f"{num(lo)} Hz–{num(hi / 1000)} kHz"
    unit = {"st": " semitones", "ct": " cents", "x": "×", "s": " s", "ms": " ms", "dB": " dB", "Hz": " Hz"}.get(u, " " + u if u else "")
    sep = "…" if lo < 0 else "–"
    return f"{num(lo)}{sep}{num(hi)}{unit}"


def describe(key, ctx):
    entries = DESC.get(key)
    if not entries:
        return None
    for pattern, long_name, text in entries:
        if pattern is None or re.match(pattern, ctx):
            return long_name, text
    return None


def glossary_rows(params):
    """-> list of dict(short, long, where, values, text), merged and sorted."""
    found = {}      # (short, long, text) -> {"where": {(kind, device): [pages]}, "values": set}
    missing = []

    def add(short, long_name, text, kind, device, page, values):
        e = found.setdefault((short, long_name, text), {"where": {}, "values": []})
        pages = e["where"].setdefault((kind, device), [])
        if page and page not in pages:
            pages.append(page)
        if values and values not in e["values"]:
            e["values"].append(values)

    def take(spec, kind, device, page, ctx_device=None):
        if spec["id"] < 0:
            return
        key, shown = norm_label(spec["label"], device, page)
        got = describe(key, f"{ctx_device or device}/{page}")
        if got is None:
            missing.append(f"{spec['label']} ({device}/{page})")
            return
        add(shown, got[0], got[1], kind, device, norm_page(page), values_text(spec))

    for eng in params["engines"]:
        device = ENGINE_NAMES[eng["name"]]
        for page in eng["pages"]:
            for spec in page["specs"]:
                take(spec, "engine", device, page["name"])
    for model, _, labels in MACRO_MODELS[1:]:
        for label in labels.split():
            got = describe(label, "Macro/MACRO")
            if got is None:
                missing.append(f"{label} (Macro model {model})")
            else:
                add(label, got[0], got[1], "engine", "Macro", "MACRO", "0–100")
    for block in params["blocks"]:
        for spec in block["specs"]:
            take(spec, "trkfx", "TRK FX", block["name"], ctx_device="TRK FX " + block["name"])
    for fx in params["effects"]:
        for spec in fx["specs"]:
            take(spec, "insert", fx["name"], "FX", ctx_device="Insert " + fx["name"])
    if missing:
        raise SystemExit("glossary_data.py has no description for:\n  " + "\n  ".join(sorted(set(missing))))

    rows = []
    for (short, long_name, text), e in found.items():
        by_pages = {}
        inserts = []
        for (kind, device), pages in e["where"].items():
            if kind == "insert":
                inserts.append(device)
            else:
                by_pages.setdefault(", ".join(pages), []).append(device)
        parts = [f"{', '.join(devs)}: {pages}" for pages, devs in by_pages.items()]
        if inserts:
            parts.append("Insert effect: " + ", ".join(inserts))
        values = e["values"][0] if len(e["values"]) == 1 else "varies"
        rows.append(dict(short=short, long=long_name, where="; ".join(parts), values=values, text=text))
    for short, long_name, device, page, values, text in EXTRA:
        rows.append(dict(short=short, long=long_name, where=f"{device}: {page}", values=values, text=text))
    rows.sort(key=lambda r: (re.sub(r"[^A-Z0-9]", "", r["short"].upper()), r["where"]))
    return rows


def glossary_html(rows):
    out = ['<table class="gloss"><thead><tr><th>Short</th><th>Full name</th><th>Belongs to</th><th>Values</th>'
           '<th>What it does</th></tr></thead><tbody>']
    for r in rows:
        out.append(f'<tr><td><code>{esc(r["short"])}</code></td><td>{esc(r["long"])}</td><td>{esc(r["where"])}</td>'
                   f'<td>{esc(r["values"])}</td><td>{esc(r["text"])}</td></tr>')
    out.append("</tbody></table>")
    return "".join(out)


# ---------------------------------------------------------------------------------------------
# Document
# ---------------------------------------------------------------------------------------------
CSS = r"""
@page { size: Letter; margin: 19mm 17mm 20mm 17mm;
        @bottom-center { content: counter(page); font: 8.5pt "Avenir Next", "Helvetica Neue", sans-serif; color: #6b7785; } }
@page :first { margin: 0; @bottom-center { content: none; } }
:root { --ink: #1b2430; --soft: #56616f; --line: #d5dbe3; --accent: #0f6e8c; --tint: #eef5f8; --warm: #b4541a; }
* { box-sizing: border-box; }
html { font: 9.6pt/1.45 "Avenir Next", "Helvetica Neue", Helvetica, Arial, sans-serif; color: var(--ink); }
body { margin: 0; }
h1, h2, h3 { font-family: "Avenir Next", "Helvetica Neue", sans-serif; line-height: 1.2; text-wrap: balance; }
h1 { font-size: 22pt; font-weight: 600; margin: 0 0 4mm; padding-bottom: 2.5mm; border-bottom: 1.2pt solid var(--accent);
     break-before: page; }
h1 .n { color: var(--accent); margin-right: 3mm; font-weight: 500; }
h2 { font-size: 12.5pt; font-weight: 600; margin: 6.5mm 0 2mm; break-after: avoid; color: var(--accent); }
h3 { font-size: 10pt; font-weight: 600; margin: 4mm 0 1mm; break-after: avoid; }
p { margin: 0 0 2.2mm; }
ul, ol { margin: 0 0 2.5mm; padding-left: 5.5mm; }
li { margin-bottom: 1mm; }
code, kbd { font: 500 8.6pt Menlo, Monaco, monospace; }
code { background: var(--tint); padding: 0 1.2mm; border-radius: 1mm; white-space: nowrap; }
kbd { border: 0.6pt solid #9aa5b1; border-bottom-width: 1.4pt; border-radius: 1.2mm; padding: 0 1.3mm; background: #fff;
      white-space: nowrap; font-size: 8.2pt; }
table { border-collapse: collapse; width: 100%; margin: 1.5mm 0 4mm; font-size: 8.6pt; }
th { text-align: left; font-weight: 600; font-size: 7.6pt; letter-spacing: .05em; text-transform: uppercase; color: var(--soft);
     border-bottom: 0.9pt solid var(--ink); padding: 1.2mm 2mm 1.2mm 0; }
td { border-bottom: 0.4pt solid var(--line); padding: 1.2mm 2mm 1.2mm 0; vertical-align: top; }
tr { break-inside: avoid; }
thead { display: table-header-group; }
.note, .try { border-left: 2.2pt solid var(--accent); background: var(--tint); padding: 2mm 3mm; margin: 2.5mm 0 3.5mm;
              break-inside: avoid; }
.try { border-color: var(--warm); background: #fbf2ea; }
.note b:first-child, .try b:first-child { text-transform: uppercase; letter-spacing: .05em; font-size: 7.8pt; margin-right: 1.5mm; }
.try b:first-child { color: var(--warm); } .note b:first-child { color: var(--accent); }
figure { margin: 0; }
figure.oled { width: 62mm; break-inside: avoid; }
figure.oled svg { width: 100%; display: block; }
figcaption { font-size: 7.8pt; color: var(--soft); margin-top: 1mm; }
.shots { display: flex; flex-wrap: wrap; gap: 4mm; margin: 2.5mm 0 4mm; }
.shots figure.oled { width: 56mm; }
.side { display: flex; gap: 6mm; align-items: flex-start; margin-bottom: 2mm; }
.side > div { flex: 1; min-width: 0; }
.side figure.oled { flex: none; }
.step { display: flex; gap: 3.5mm; margin: 0 0 3mm; break-inside: avoid; }
.step .k { flex: none; width: 7mm; height: 7mm; border-radius: 50%; background: var(--accent); color: #fff; font-weight: 600;
           display: flex; align-items: center; justify-content: center; font-size: 9.5pt; margin-top: .5mm; }
.step .b { flex: 1; min-width: 0; }
.step h3 { margin-top: .8mm; }
.cover { height: 279.4mm; padding: 34mm 22mm 20mm; background: #0b1d28; color: #e9f4f8; display: flex; flex-direction: column;
         break-after: page; }
.cover .eyebrow { letter-spacing: .28em; text-transform: uppercase; font-size: 9pt; color: #7fc4d8; }
.cover .title { font-size: 46pt; font-weight: 600; line-height: 1.02; margin: 7mm 0 5mm; letter-spacing: -.01em; }
.cover .sub { font-size: 13pt; max-width: 120mm; color: #bcd6df; line-height: 1.35; }
.cover figure.oled { width: 98mm; margin-top: 20mm; }
.cover .foot { margin-top: auto; font-size: 8.6pt; color: #86a4af; display: flex; justify-content: space-between; }
.toc { break-before: page; }
.toc h1 { break-before: auto; }
.toc ol { list-style: none; padding: 0; margin: 0; columns: 2; column-gap: 10mm; }
.toc li { display: flex; align-items: baseline; gap: 1.5mm; margin: 0 0 .9mm; break-inside: avoid; }
.toc li.c { font-weight: 600; margin-top: 2.6mm; }
.toc li.s { padding-left: 7mm; color: var(--soft); font-size: 8.8pt; }
.toc .dots { flex: 1; border-bottom: 0.5pt dotted #9aa5b1; transform: translateY(-1mm); }
.toc a { color: inherit; text-decoration: none; }
.mk { position: absolute; font-size: 1pt; color: #fff; }
table.gloss { font-size: 7.7pt; table-layout: fixed; }
table.gloss th:nth-child(1) { width: 17mm; } table.gloss th:nth-child(2) { width: 30mm; }
table.gloss th:nth-child(3) { width: 40mm; } table.gloss th:nth-child(4) { width: 31mm; }
table.gloss td { padding: .8mm 2mm .8mm 0; overflow-wrap: anywhere; }
table.gloss code { font-size: 7.6pt; white-space: normal; }
table.cols3 td:first-child, table.keys td:first-child { white-space: nowrap; }
table.keys td:first-child { width: 52mm; }
.diagram { margin: 2mm 0 4mm; break-inside: avoid; }
.diagram svg { width: 100%; }
.two { columns: 2; column-gap: 8mm; }
.two > * { break-inside: avoid; }
"""


def controller_svg():
    """Schematic of the control surface (not to scale)."""
    o = ['<svg viewBox="0 0 720 300" font-family="Avenir Next, Helvetica Neue, sans-serif" font-size="10">',
         '<rect x="4" y="4" width="712" height="292" rx="12" fill="#f4f6f8" stroke="#56616f"/>']

    def btn(x, y, w, h, label, fill="#fff", fs=8.5):
        o.append(f'<rect x="{x}" y="{y}" width="{w}" height="{h}" rx="3" fill="{fill}" stroke="#56616f"/>')
        o.append(f'<text x="{x + w / 2}" y="{y + h / 2 + 3}" text-anchor="middle" font-size="{fs}">{label}</text>')

    # strips
    for i, name in enumerate(("Pitch", "Mod")):
        o.append(f'<rect x="{22 + i * 30}" y="26" width="20" height="92" rx="4" fill="#dfe5ea" stroke="#56616f"/>')
        o.append(f'<text x="{32 + i * 30}" y="132" text-anchor="middle" font-size="8.5">{name}</text>')
    btn(20, 142, 52, 18, "Shift", "#e3f1f6")
    btn(20, 164, 24, 18, "Oct−"); btn(48, 164, 24, 18, "Oct+")
    # display
    o.append('<rect x="92" y="26" width="110" height="56" rx="4" fill="#0b0d10"/>')
    o.append('<text x="147" y="58" text-anchor="middle" fill="#bfe9ff" font-size="9">OLED 128 × 64</text>')
    btn(92, 92, 34, 18, "Settings", fs=7.5); btn(130, 92, 34, 18, "Arp"); btn(168, 92, 34, 18, "Scale")
    # knobs
    for i in range(8):
        cx = 245 + i * 50
        o.append(f'<circle cx="{cx}" cy="52" r="15" fill="#fff" stroke="#56616f"/>')
        o.append(f'<text x="{cx}" y="56" text-anchor="middle">{i + 1}</text>')
    btn(640, 30, 28, 18, "▲"); btn(640, 54, 28, 18, "▼")
    o.append('<text x="676" y="43" font-size="8">knob</text><text x="676" y="54" font-size="8">page</text>')
    # pads
    btn(206, 104, 20, 30, "▲"); btn(206, 142, 20, 30, "▼")
    o.append('<text x="216" y="98" text-anchor="middle" font-size="7.5">Track</text>')
    for r in range(2):
        for i in range(8):
            x, y = 232 + i * 50, 100 + r * 40
            o.append(f'<rect x="{x}" y="{y}" width="44" height="34" rx="4" fill="#fff" stroke="#56616f"/>')
            o.append(f'<text x="{x + 22}" y="{y + 21}" text-anchor="middle" fill="#56616f">{i + 1}</text>')
    btn(636, 104, 30, 30, "&gt;", "#e3f1f6"); btn(636, 142, 30, 30, "Func", "#e3f1f6")
    btn(672, 104, 34, 30, "Play"); btn(672, 142, 34, 30, "Rec")
    # keys
    whites = 15
    w = 680 / whites
    for i in range(whites):
        o.append(f'<rect x="{20 + i * w}" y="196" width="{w}" height="88" fill="#fff" stroke="#56616f"/>')
    for i in range(whites - 1):
        if i % 7 in (0, 1, 3, 4, 5):
            o.append(f'<rect x="{20 + (i + 1) * w - 7}" y="196" width="14" height="54" fill="#1b2430"/>')
    o.append('<text x="360" y="190" text-anchor="middle" font-size="8.5" fill="#56616f">25 keys (schematic; positions are not to scale)</text>')
    o.append("</svg>")
    return "".join(o)


def build_html(params, screens, pages=None):
    """pages: {anchor: page number} from the first pass, or None for the first pass (markers on)."""
    ctx = dict(S=screens, params=params, controller_svg=controller_svg(), fx_names=FX_NAMES, macro_models=MACRO_MODELS,
               glossary=glossary_html(glossary_rows(params)))
    body, toc = [], []
    for ci, (title, sections) in enumerate(chapters.build(ctx)):
        number = chapters.chapter_number(ci)
        anchor = f"c{ci}"
        mark = "" if pages else f'<span class="mk">@@{anchor}@@</span>'
        body.append(f'<h1 id="{anchor}">{mark}<span class="n">{number}</span>{esc(title)}</h1>')
        toc.append(("c", anchor, f"{number}  {title}"))
        for si, (stitle, content) in enumerate(sections):
            if stitle:
                a2 = f"{anchor}s{si}"
                mark = "" if pages else f'<span class="mk">@@{a2}@@</span>'
                body.append(f'<h2 id="{a2}">{mark}{esc(stitle)}</h2>')
                toc.append(("s", a2, stitle))
            body.append(content)
    items = []
    for kind, anchor, text in toc:
        page = pages.get(anchor, "") if pages else ""
        items.append(f'<li class="{kind}"><a href="#{anchor}">{esc(text)}</a><span class="dots"></span><span>{page}</span></li>')
    cover = (f'<section class="cover"><div class="eyebrow">Owner\'s manual</div><div class="title">Neptune<br>Groovebox</div>'
             f'<div class="sub">An eyes-free groovebox played entirely from a Launchkey Mini 25 MK4: eight sound engines, '
             f'a step sequencer with parameter locks, per-pad pressure, and sampling.</div>'
             f'{screens.svg("TRACKS", 1)}'
             f'<div class="foot"><span>{esc(chapters.EDITION)}</span><span>Firmware NPE · macOS</span></div></section>')
    return (f'<!doctype html><html><head><meta charset="utf-8"><title>Neptune Groovebox Manual</title><style>{CSS}</style></head>'
            f'<body>{cover}<section class="toc"><h1>Contents</h1><ol>{"".join(items)}</ol></section>{"".join(body)}</body></html>')


def render(html_text, pdf_path):
    with tempfile.NamedTemporaryFile("w", suffix=".html", delete=False, encoding="utf-8") as f:
        f.write(html_text)
    subprocess.run([CHROME, "--headless", "--disable-gpu", "--no-pdf-header-footer", f"--print-to-pdf={pdf_path}",
                    "file://" + f.name], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    os.unlink(f.name)


def main():
    params_path, screens_dir, out = sys.argv[1:4]
    params = json.load(open(params_path))
    screens = Screens(screens_dir)
    tmp = tempfile.mktemp(suffix=".pdf")
    render(build_html(params, screens), tmp)
    text = subprocess.run(["pdftotext", tmp, "-"], capture_output=True, text=True).stdout
    pages = {}
    for i, page in enumerate(text.split("\f")):
        for anchor in re.findall(r"@@(c\d+(?:s\d+)?)@@", page):
            pages.setdefault(anchor, i + 1)
    os.unlink(tmp)
    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    render(build_html(params, screens, pages), out)
    print(f"{out}: {len(text.split(chr(12))) - 1} pages, {len(pages)} headings placed")


if __name__ == "__main__":
    main()
