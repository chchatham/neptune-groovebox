"""The manual's prose. build(ctx) -> [(chapter title, [(section title or None, html)])]."""
import html
import re

from glossary_data import DESC, DESTS

EDITION = "Version 0.9, November 2026"

NUMBERED = 12          # chapters 1..12, then appendices A, B, C, D


def chapter_number(index):
    return str(index + 1) if index < NUMBERED else "Appendix " + "ABCD"[index - NUMBERED]


def fmt(text):
    """[[Shift]] -> <kbd>, {{CUT}} -> <code>."""
    text = re.sub(r"\[\[(.+?)\]\]", lambda m: f"<kbd>{m.group(1)}</kbd>", text)
    return re.sub(r"\{\{(.+?)\}\}", lambda m: f"<code>{html.escape(m.group(1))}</code>", text)


def step(n, title, body):
    return f'<div class="step"><div class="k">{n}</div><div class="b"><h3>{title}</h3>{body}</div></div>'


def table(head, rows, cls=""):
    h = "".join(f"<th>{c}</th>" for c in head)
    b = "".join("<tr>" + "".join(f"<td>{c}</td>" for c in r) + "</tr>" for r in rows)
    return f'<table class="{cls}"><thead><tr>{h}</tr></thead><tbody>{b}</tbody></table>'


def choices(params, engine, page, label):
    for e in params["engines"]:
        if e["name"] == engine:
            for p in e["pages"]:
                if p["name"] == page:
                    for s in p["specs"]:
                        if s["label"] == label:
                            return s["choices"]
    raise SystemExit(f"no choices for {engine}/{page}/{label}")


# The synth's DEST list (its MOD page is built by the screen, so it is not in the parameter dump;
# the "LFO Destinations" unit tests check the same names against the glossary).
SYNTH_DESTS = ("PTCH CUT RESO MIX AMP PW XMOD CUTB PAN LVL1 LVL2 LVL3 NOIS FFM PIT2 RAT1 RAT2 RAT3 RAT4 DPT1 DPT2 DPT3 DPT4 | "
               "TUN2 DET2 DRIV TUN3 NCOL | 1A 1B 2A 2B 3A 3B | SPRD GLID DRFT DRAT | ENV FDRV MORP RESB BAL DRVB TRKB | "
               "FATK FDEC FSUS FREL KEY FVEL PMOD | ATK DEC SUS REL VEL | ATK2 DEC2 SUS2 REL2 | ATK3 DEC3 SUS3 REL3 | "
               "ATK4 DEC4 SUS4 REL4 | ATK5 DEC5 SUS5 REL5 EDPT")


def dest_cell(labels):
    """The DEST list as text; every name must be described in the glossary data."""
    missing = [l for l in labels if l != "|" and not any(k in DESC or k in DESTS for k in (l, re.sub(r"\d+", "#", l)))]
    if missing:
        raise SystemExit("glossary_data.py describes no LFO destination called: " + " ".join(sorted(set(missing))))
    count = sum(1 for l in labels if l != "|")
    return f"{count}: " + " ".join("<b>·</b>" if l == "|" else f"<code>{html.escape(l)}</code>" for l in labels)


def page_list(params, engine):
    for e in params["engines"]:
        if e["name"] == engine:
            rows, seen = [], set()
            for p in e["pages"]:
                name = re.sub(r"^DR \d+$", "DR", p["name"])
                name = re.sub(r"^OP\d$", "OP1–OP6", name)
                if name in seen:
                    continue
                seen.add(name)
                rows.append((f"<code>{name}</code>", " ".join(s["label"] for s in p["specs"][:4] if s["id"] >= 0),
                             " ".join(s["label"] for s in p["specs"][4:] if s["id"] >= 0)))
            return table(["Page", "Knobs 1–4", "Knobs 5–8"], rows, "cols3")
    raise SystemExit(engine)


def build(ctx):
    S, params = ctx["S"], ctx["params"]
    ch = []

    # =========================================================================================
    ch.append(("Welcome", [
        (None, fmt("""
<p>Neptune Groovebox turns a Novation Launchkey Mini 25 MK4 into a self-contained instrument. The Mac makes the sound;
everything you play, edit, sequence and save is done from the Launchkey, guided by its small display. You never need to
look at the Mac.</p>
<p>The design borrows the working habits of hardware grooveboxes: a step sequencer where any step can hold its own
value for any knob, a handful of modifier gestures that mean the same thing on every screen, and patterns that you
chain into songs.</p>""")),
        ("What is inside", fmt("""
<ul>
<li><b>Eight sound engines</b>, one per track: a configurable analog-style synth, six-operator FM, a macro oscillator
with eight models, wavetable, wave sequencer, sampler with slicing, time stretch and a granular mode, a 16-voice drum kit, and a MIDI track for
outside gear.</li>
<li><b>Five filter models</b> with a live picture of the response curve.</li>
<li><b>Modulation</b>: per-voice LFOs with nine shapes and five trigger modes, extra envelopes, and pad pressure routed
per track and per note.</li>
<li><b>Effects</b>: three character blocks on every track (DIRT, TALK, MODFX), insert slots with 30 effect types, two
send buses, and a master EQ, compressor and limiter.</li>
<li><b>Sequencer</b>: 16 patterns, per-track length and speed, parameter locks, trig conditions, micro-timing, live
recording, song mode.</li>
<li><b>An arpeggiator on every track</b>, with variation and mutation, and pressure-driven rolls on the pads.</li>
<li><b>Resampling</b>: record the master or any track into a new sample, then play it, granulate it or turn it into a
wavetable.</li>
</ul>""")),
        ("How to read this manual", fmt("""
<p>Buttons are shown like [[Shift]]; names you will see on the display are shown like {{CUT}}. "Top pad 3" means the
third pad from the left in the top row. Knobs are numbered 1 to 8 from the left.</p>
<p>Chapter 2 is a guided first session that ends with a short song. Chapters 3 to 12 are reference. Appendix A lists
every short name on the display with its full name, where it lives and what it does.</p>
<div class="note"><b>Status</b>This manual describes the software as built. Much of it has so far been checked by
automated tests rather than by ear and hand; where the instrument and the manual disagree, the instrument is right and
the manual wants correcting.</div>""")),
    ]))

    # =========================================================================================
    qs = []
    qs.append((None, fmt("""
<p>About half an hour, from silence to a two-pattern song. Each step names the screen you should be on; if you get
lost, hold [[Shift]] and the pads light up as a map of the screens.</p>
<div class="try"><b>If you forget a combination, hold the key</b>Hold [[Shift]], [[Func]], [[&gt;]], [[▲]] or
[[Rec]] by itself for a third of a second and the display turns into a help card: what that key does with the pads,
the buttons and the knobs on the screen you are on. The two small knob-page buttons turn the cards. Hold [[Func]] to read what copy, clear and paste
would act on right now, before you press anything. Any other control makes the card step aside and does its job as
usual.</div>""")))
    qs.append(("Before you start", fmt("""
<ul>
<li>Plug the Launchkey into the Mac. It needs the Neptune firmware (tag NPE), which is already on your unit.</li>
<li>Start <i>Neptune Groovebox</i>. The display shows the {{TRACKS}} screen within a couple of seconds.</li>
<li>Use headphones or an audio interface if you can. The Mac's built-in speakers add a noticeable delay of their own.</li>
<li>The app reopens the project you last used. For a clean start: [[Settings]], press the lower knob-page button
twice to reach {{PROJ}}, and turn {{NEW}} (knob 5) four clicks clockwise.</li>
</ul>""") + ctx_diagram(ctx)))

    a = []
    a.append(step(1, "Look around", fmt("""
<p>Hold [[Shift]]. The pads show where each screen lives: the bright pad is where you are. Still holding
[[Shift]], press top pad 2 to open {{TRACKS}}, the mixer. Knob 1 is the volume of track 1, knob 2 of track 2, and so on.</p>
<p>The two small buttons beside the knobs turn the knob page on every screen. On {{TRACKS}} the pages are
{{VOL}}, {{PAN}}, {{SND1}}, {{SND2}} and {{ENG}}. The pages go round: the upper button on the first page gives the
last one, and the lower button on the last page gives the first.</p>""")))
    a.append(step(2, "Play a sound and choose a preset", fmt("""
<p>Play the keys: track 1 is a synth. [[Shift]] + top pad 5 opens {{PRESET}}. Scroll with knob 1 ({{PS}}) and play:
each preset you land on takes over the track's sound, for listening only. Knob 2 ({{CT}}) narrows the list to basses,
pads, leads and so on. When you like one, turn knob 5 ({{LD}}) four clicks clockwise to keep it. Leave without
loading and your previous sound comes back.</p>""")))
    a.append(step(3, "Shape it", fmt("""
<p>[[Shift]] + top pad 1 opens {{PARAM}}, the pages of the track's sound engine. It opens on {{MAIN}}: hold a note
and turn {{CUT}} (knob 1) and {{RESO}} (knob 2), and a picture of the filter's response appears while you turn.
Knobs 5 to 8 are the envelope ({{ATK DEC SUS REL}}), with a picture of its shape. The knob-page buttons step on
to the engine's other pages; the full filter is on {{FILTR}}.</p>
<p>Three speeds work on every knob: plain, [[Shift]] held for fine, [[Func]] held for coarse jumps to useful values.</p>""")
        + '<div class="shots">' + S.svg("PARAM synth", 1, "PARAM opens on MAIN") + S.svg("FLTR", 1, "FLTR: the same filter, drawn large") + "</div>"))
    a.append(step(4, "Record a drum beat", fmt("""
<p>Track 2 is a drum kit. Hold [[▼]] (the lower of the two Track buttons left of the pads) and tap top pad 2 to select
track 2, then release. [[Shift]] + bottom pad 2 opens {{DRUMS}}: the sixteen pads are sixteen drum voices.</p>
<p>In the starting kit the bottom row holds three kicks (pads 1 to 3), a tom, a snare (pad 5), a clap, and closed and
open hats (pads 7 and 8). The top row holds six tuned sub-bass hits and two cymbals.</p>
<p>Hold [[Rec]] and press [[Play]]: the sequencer starts from the top, recording. The name bar says
<code>REC FREE</code> and a red light runs across the pads, one pad per step, while the pads go on playing the
drums. Tap a beat: a kick on bottom pad 1, the snare on pad 5, hats on pad 7. The pattern loops after 16 steps and
what you played comes round again.</p>
<p>Press [[Play]] to stop. Recording ends and {{STEP}} opens with the drum track selected: your beat is laid out
on the pads, ready for editing. Press [[Play]] again to hear it.</p>
<p>Free recording keeps your timing exactly. To tighten it afterwards, raise {{QN}} on {{STEP}} (knob 3): playback
is pulled toward the grid without changing what you recorded. To record straight onto the grid instead, press
[[Rec]] + [[Play]] once more while recording: the name bar changes to <code>REC QUANT</code>.</p>""")))
    a.append(step(5, "Add a bassline with steps", fmt("""
<p>Hold [[▼]], tap top pad 1, release: track 1 again. Open {{STEP}} ([[Shift]] + top pad 4). The sixteen pads are now
the sixteen steps, top row first. Tap pads to place notes; a quick tap on a lit step removes it.</p>
<p>To choose a step's note, hold its pad and press a key. Do that for each step and you have a bassline.</p>""")
        + '<div class="shots">' + S.svg("STEP", 1, "STEP") + "</div>"))
    a.append(step(6, "Make one step different", fmt("""
<p>This is the heart of the instrument. Go to {{PARAM}} and the {{FILTR}} page. The pads still show the steps. Hold one
lit step and turn {{CUT}}: only that step changes, and its pad turns orange. That is a <i>parameter lock</i>. Any knob
on any sound page can be locked to any step.</p>
<p>To remove one lock, hold the step, hold [[&gt;]], and turn the same knob.</p>""")))
    a.append(step(7, "Add space", fmt("""
<p>[[Shift]] + top pad 3 opens {{TRK FX}}. On its first page, {{SND1}} sends the track to a reverb and {{SND2}} to a
delay. A little reverb on the snare track, a little delay on the bass.</p>""")))
    a.append(step(8, "Make a second pattern", fmt("""
<p>Open {{TRACKS}}. Press [[Func]] + [[Rec]] to copy the whole pattern. Hold [[&gt;]] and tap top pad 2 to move to
pattern 2, then press [[Func]] + [[&gt;]] to paste. Now change something in pattern 2: remove some bass steps, lock a
few filter values, mute the hats by re-recording. Hold [[&gt;]] and tap pad 1 or 2 to switch between the two while
playing; the change waits for the end of the loop.</p>""")))
    a.append(step(9, "Chain them into a song", fmt("""
<p>[[Shift]] + top pad 6 opens {{SONG}}. Turn {{PA}} (knob 1): row 1 appears, playing pattern 1. Set {{RP}} (knob 2)
to 2 repeats. Tap the pad after the last row to add row 2, and set its {{PA}} to pattern 2. Turn {{SG}} (knob 6) on
and {{LP}} (knob 5) on, then press [[Play]]: the song plays pattern 1 twice, pattern 2, and loops.</p>""")
        + '<div class="shots">' + S.svg("SONG", 1, "SONG") + "</div>"))
    a.append(step(10, "Save", fmt("""
<p>[[Settings]], then the {{PROJ}} page. Choose a {{SLOT}} with knob 1 and turn {{SVAS}} (knob 4) four clicks
clockwise. The display answers OK. Next time the app starts it reopens this project.</p>""")))
    qs.append(("Ten steps to a song", "".join(a)))

    qs.append(("Where to go next", fmt("""
<p>Each of these is a few minutes of play and opens a different side of the instrument.</p>
<div class="two">
<div class="try"><b>Squeeze</b>Open {{SCALE}} ([[Shift]] + bottom pad 4), hold a pad and lean into it. Then open
{{AT ROUTE}} ([[Shift]] + top pad 8) and choose what pressure does on this track. Each finger has its own pressure.
Chapter 7.</div>
<div class="try"><b>Pressure on any knob</b>On {{PARAM}}, turn a knob, then press [[Func]] + [[▲]]. The bottom pad
under that knob now sweeps it with pressure and lets go on release. Chapter 7.</div>
<div class="try"><b>Eat your own tail</b>Open {{WAVE}} ([[Shift]] + bottom pad 5), go to its {{REC}} page and record
the master output for a bar. The take becomes a sample you can play on a sampler track. Chapter 11.</div>
<div class="try"><b>Freeze time</b>On a sampler track set {{MODE}} to {{GRAN}} and {{SCAN}} to 0 on the {{GRAIN}}
page. The sound stands still; move {{STRT}} to walk through it by hand. Chapter 11.</div>
<div class="try"><b>Any loop, your tempo</b>Put a loop on a sampler track, set {{MODE}} to {{LOOP}}, then on
{{STRCH}} choose an {{ALGO}}, switch {{SYNC}} on and set {{LEN}}. Change the tempo: it follows. Try each
algorithm on the same loop. Chapter 11.</div>
<div class="try"><b>Chop a break</b>On {{SLICE}} set {{MODE}} to {{TRNS}} and {{PLAY}} to {{KEYS}}: every hit of the
loop is now on its own key. Or {{PLAY}} = {{RAND}} with steps on every sixteenth. Chapter 11.</div>
<div class="try"><b>A wavetable from anything</b>On a wavetable track set {{TABL}} to {{USER}}, then choose a recording
with {{SM}} on the {{WAVE}} screen's {{REC}} page. {{POS}} now scans through your recording, and the {{SCAN}} page can play it
back at any length. Chapter 5.</div>
<div class="try"><b>Patterns that never repeat</b>Give two tracks different lengths ({{LN}} on {{STEP}}): 16 against
12, say. Then give some steps a condition ({{COND}}: {{PROB}} or {{A:B}}). Chapter 9.</div>
<div class="try"><b>An arp that evolves</b>Press [[Arp]] twice and hold a chord. On the {{VARY}} page raise
{{SPCE}}; on {{MUTA}} switch mutation on and let it run. When you like what it has become, {{>PAT}} writes it into
the pattern. Chapter 10.</div>
<div class="try"><b>Rolls under your finger</b>On the {{ARP}} screen's {{ROLL}} page switch {{ROLL}} on. Now a held
drum pad repeats, faster the harder you press. Chapter 10.</div>
<div class="try"><b>A synth that talks</b>{{TRK FX}}, {{TALK}} page: raise {{MIX}}, sweep {{VOWL}}, add {{DPTH}}.
Chapter 8.</div>
<div class="try"><b>Pumping</b>Put the {{DUCK}} effect on a bass or pad track and set its {{SRC}} to the drum
track. Chapter 8.</div>
<div class="try"><b>Every note slightly different</b>On the synth's {{ANALG}} page raise {{SLOP}}, {{FVAR}} and
{{EVAR}}. On drums, point an LFO at {{TONE}} with {{TRIG}} = {{HOLD}} and {{SHPE}} = {{RND}}. Chapters 5 and 7.</div>
<div class="try"><b>Jam without fear</b>Hold [[&gt;]] and press [[Rec]] to keep a copy of the pattern. Wreck it.
Hold [[&gt;]] and press [[Play]] to bring it back. Chapter 9.</div>
<div class="try"><b>All tracks at once</b>Hold [[▲]] or [[▼]] and turn a knob: the same control moves on every track
that has it. Sweep every filter together. Chapter 3.</div>
<div class="try"><b>Two hands, two registers</b>[[Shift]] + [[Oct−]] drops the lowest octave of the keyboard by a
further octave or more: bass in the left hand, chords in the right. Chapter 12.</div>
</div>""")))
    ch.append(("Quick start", qs))

    # =========================================================================================
    ch.append(("Finding your way", [
        ("The control surface", ctx_diagram(ctx) + fmt("""
<ul>
<li><b>Eight endless knobs.</b> They always edit the eight things shown on the display, in the same positions.</li>
<li><b>Knob-page buttons</b> (the small pair right of the knobs): previous and next page of knobs, on every screen.
The pages go round, so from the first page the upper button reaches the last in one press.</li>
<li><b>Sixteen pads</b>, velocity- and pressure-sensitive. What they do depends on the screen: play notes, hold
steps, select rows.</li>
<li><b>[[▲]] [[▼]]</b> left of the pads (printed <i>Track</i>): tapped, they change what the pads address (the next
sixteen steps, the next scale page, the next row). Held, they open the Track layer.</li>
<li><b>[[&gt;]]</b> right of the top pad row: the pattern key.</li>
<li><b>[[Func]]</b> right of the bottom pad row: "bigger, or secondary".</li>
<li><b>[[Shift]]</b>: "where, and how finely".</li>
<li><b>Any of these held alone</b> ([[Shift]], [[Func]], [[&gt;]], [[▲]] or [[▼]], [[Rec]]) shows its help cards
on the display (see <i>Help cards</i> below).</li>
<li><b>[[Settings]] [[Arp]] [[Scale]] [[Play]] [[Rec]]</b> do what they say. <b>[[Oct−]] [[Oct+]]</b> shift the keys.</li>
<li><b>Pitch strip and mod strip</b> act on the selected track.</li>
</ul>""")),
        ("The display", '<div class="side"><div>' + fmt("""
<p>The top line is the name bar: the screen or page name on the left, the selected track ({{T 1}}) on the right, and
tags when something is engaged: {{ARP}}, {{LATCH}}, {{PIN}}, {{SPLIT}}.</p>
<p><b>Grid screens</b> show eight cells in two rows of four. Knobs 1 to 4 are the top row, 5 to 8 the bottom row.</p>
<p><b>Bar screens</b> show a picture or list with a strip of eight two-letter labels along the bottom, one per knob.
When you turn a knob the name bar spells out its full name and value. A knob that has nothing to act on at the
moment says why there for a second instead of doing nothing: {{SAMPLER ONLY}}, {{NOT ZOOMED}} and {{ONE SLICE}} on
{{WAVE}}, {{ROUTE OFF}} on {{AT ROUTE}}, {{FLT B IS OFF}}, {{FILTER IS OFF}} and {{SEM MRPH ONLY}} on the filter pages,
{{SLOT IS EMPTY}} on {{TRK FX}}, {{NOTHING KEPT}} and {{NOTHING TAPPED}} on {{ARP}}, {{SONG IS EMPTY}} and
{{ONE ROW}} on {{SONG}}, {{NO LOCKS}} on a held step.</p>""") + "</div>"
            + S.svg("PARAM synth", 2, "A grid screen: the synth's OSC page") + "</div>"
            + '<div class="shots">' + S.svg("TRACKS", 1, "A bar screen: TRACKS, VOL page") + S.svg("ARP", 1, "ARP, first page") + "</div>"),
        ("Screens: Shift + pad", fmt("""
<p>Hold [[Shift]] and press a pad. The screens follow the legends printed above and below the pads.</p>""")
            + table(["Pad", "Top row", "Bottom row"], [
                ("1", fmt("{{PARAM}} — the sound engine's pages"), fmt("{{STEP}} — the step sequencer")),
                ("2", fmt("{{TRACKS}} — mixer and engine choice"), fmt("{{DRUMS}} — drum pads")),
                ("3", fmt("{{TRK FX}} — track effects"), fmt("{{ARP}} — arpeggiator and rolls")),
                ("4", fmt("{{STEP}} — the step sequencer"), fmt("{{SCALE}} — scale pads")),
                ("5", fmt("{{PRESET}} — sound presets"), fmt("{{WAVE}} — waveform and resampling")),
                ("6", fmt("{{SONG}} — song rows"), fmt("{{FLTR}} — the filter picture")),
                ("7", fmt("{{SETTINGS}} (also the [[Settings]] button)"), "—"),
                ("8", fmt("{{AT ROUTE}} — pad pressure routing"), "—")])),
        ("The grammar: one gesture, one meaning", fmt("""
<p>These hold on every screen. Learn this table and you know the instrument.</p>""")
            + table(["Hold", "and turn a knob", "and press a pad", "and press ▲ / ▼", "and press a knob-page button"], [
                ("nothing", "edit, one step", "the screen's pad action", "what the pads address: step page, scale page, song row, preset list", "knob page ±1"),
                (fmt("[[Shift]]"), fmt("fine: a tenth of a step (on an envelope's {{ATK}}, {{DEC}} or {{REL}}: that stage's curve, see Filters)"), "go to that screen", "previous / next track", "—"),
                (fmt("[[Func]]"), "coarse: jump to useful values", fmt("top row: jump to a page; bottom row on {{TRACKS}}: solo"),
                 fmt("set the low ([[▼]]) or high ([[▲]]) pressure-range end of the knob turned last"), "section jump, or first / last page"),
                ("a step pad", "lock that knob on that step", "—", "—", "lock page ±1"),
                (fmt("[[&gt;]]"), "on a held step: remove that knob's lock", "queue pattern 1–16", "—", fmt("([[Rec]] keeps a copy of the pattern, [[Play]] reloads it)")),
                (fmt("[[▲]] or [[▼]]"), "the same control on every track", "top row: select track N; bottom row: mute track N", "—", "—")])
            + fmt("""
<h3>Knob speeds</h3>
<p>Plain is 1 % of a continuous range or one item of a list. [[Shift]] is a tenth of that. [[Func]] snaps: levels to
0, 25, 50, 75, 100 % and the default; two-sided values to the ends, the centre and the quarters; tuning and
frequencies by octaves; tempo by 10; lengths by 4; division lists to the straight (not dotted or triplet) values;
an LFO's {{DEST}} list from one page's group of destinations to the next.</p>
<h3>Buttons</h3>""")
            + table(["Button", "Plain", "With Shift", "With Func"], [
                (fmt("[[Arp]]"), fmt("Opens {{ARP}}; pressed again there, switches the track's arp on or off"), "Latch", "—"),
                (fmt("[[Scale]]"), "Scale lock on / off (the name bar says which, with the scale)", fmt("Opens {{SCALE}}"), "—"),
                (fmt("[[Settings]]"), fmt("Opens {{SETTINGS}}"), "—", "Tap tempo"),
                (fmt("[[Play]]"), "Start / stop; stopping also ends recording", "Stop and return to the start", "Clear"),
                (fmt("[[Rec]]"), fmt("Recording on / off, when the button is let go. Held + [[Play]]: start recording; again: free / quantized"), "Fill, while held", "Copy"),
                (fmt("[[&gt;]]"), "Pattern key (hold)", "—", "Paste")], "keys")
            + fmt("""
<h3>Help cards</h3>
<p>Every key that changes what the other controls do while you hold it has help cards: [[Shift]], [[Func]],
[[&gt;]], [[▲]] / [[▼]] and [[Rec]]. Hold one of them by itself. After a third of a second the display becomes a
help card for that key, and stays for as long as you hold it. While a card is showing, the two knob-page buttons turn to the key's other cards
(the mark at the top right, such as {{2/3}}, says which one you are on). Anything else you press or turn makes the
card step aside for the rest of that hold and then does exactly what it always does, so a quick [[Shift]] + pad or
[[Func]] + [[Play]] never sees a card at all. Let go and the screen is back as it was.</p>
<p>[[Shift]] has three cards, the same everywhere: the pads (a map of the screens, the one you are on inverted),
the buttons, and the knobs. [[Func]] has three that follow the screen: <i>edit</i> (what [[Rec]], [[Play]] and
[[&gt;]] would copy, clear and paste right now, and what undo and redo would take back or do again), the pads (the
page keys by name), and the knobs and other buttons.
{{HELP}} on {{SETTINGS}} → {{SETUP}} chooses when the cards appear: {{OFF}}, {{0.35S}} or {{INST}} (at once).</p>
<p>[[&gt;]] has two cards: the sixteen patterns laid out like the pads (the current one inverted, the one queued
next marked {{*}}, patterns with steps in them by number, empty ones as a dot), and what [[Rec]], [[Play]] and a
knob do under it, including whether [[Play]] would reload a kept copy or the pattern as the project had it.
[[▲]] / [[▼]] have two: the eight tracks over the top pads with their engines and, under each, {{ON}}, {{MUTE}} or
{{SOLO}}; and what a knob does under them here and what a quick press of the buttons moves on this screen.
[[Rec]] has one: what letting go will do, and the chord with [[Play]].</p>
<p>Two things to know. A quick press of [[▲]] or [[▼]] still pages as always; a press held long enough to show the
card does not page when you let go, because the card took its place. [[Rec]] is the exception: it switches
recording on or off when you let go whether or not its card was up. [[Play]], [[Settings]], [[Arp]] and [[Scale]]
act the moment you press them and have no cards; what they do with [[Shift]] or [[Func]] is on those keys' cards.</p>
<h3>Copy, clear, paste</h3>
<p>[[Func]] + [[Rec]] copies, [[Func]] + [[Play]] clears, [[Func]] + [[&gt;]] pastes. What they act on follows where
you are: a held step if you are holding one ({{STEP}}: the whole step; {{PARAM}}, {{TRK FX}}, {{FLTR}}: its locks);
otherwise the track's steps on {{STEP}}, the whole pattern on {{TRACKS}}, the track's sound on {{PARAM}} and
{{FLTR}}, and the track's effects on {{TRK FX}}. On {{PARAM}} and {{FLTR}} copy and paste take the whole sound
(engine, mixer, effects), and clear resets the engine's own parameters and leaves the mixer and the effects alone, so
the display calls it {{RESET}}. On {{TRK FX}} all three act on the effects only: the {{DIRT}}, {{TALK}} and {{MODFX}}
blocks, the insert slots and the {{ARPLK}} values. Clearing switches the blocks off, empties the slots and sets the
arp locks to off; volume, pan, sends, length and speed on the {{TRACK}} page are mixing and sequencing and are never
touched. On other screens the three do nothing, and the name bar says so ({{NO CLEAR HERE}}). The name bar confirms
what was done ({{CLEAR T3 STEPS}}, {{CLEAR T3 FX}}). If you are not sure what a combination will act on, hold [[Func]]
and read the card first.</p>
<p><b>A whole pattern needs a held button.</b> On {{TRACKS}}, clear and paste replace every track's steps, so
[[Func]] + [[Play]] and [[Func]] + [[&gt;]] act only after the button has been held for half a second. The name bar
(or the card, if it is showing) fills up while you hold: {{CLEAR >>>..}}. Let go early and nothing happens; the name
bar says {{HOLD TO CLEAR}}. Everything smaller acts at the press.</p>
<h3>Undo and redo</h3>
<p>Hold [[Func]] and [[Shift]] and press [[Play]] to undo; hold them and press [[Rec]] to redo. The last fifteen
actions can be taken back, one press each, and it does not matter what you played, turned or looked at in between:
an action stays undoable until it is undone or a sixteenth pushes it out. The name bar says what was taken back
({{UNDO CLEAR T3 STEPS}}), and the [[Func]] card shows what the next undo and redo would be before you press
({{UNDO: CLEAR T3 STEPS}}, {{REDO: ...}}, or {{NOTHING TO UNDO}}).</p>
<p>These are undo steps: every clear, paste and reset; reloading a pattern with [[&gt;]] + [[Play]]; changing an
insert's {{TYPE}}; {{LCKS}} clearing a step's locks; {{EVOL}} turned back to the ancestor; {{>PAT}}; and a recording,
one step from the moment it starts to the moment it stops, so a bad take is one press away from gone (also while it
is still running: the take so far goes and recording carries on). Knob turns are <b>not</b> undo steps. Undoing a
change to a sound puts back what that action changed and leaves knobs you turned since as they are. Loading a
project or starting a new one empties the list, and a step whose track has been given another engine since is
skipped.</p>
<p>The older shortcut still works: the same copy, clear or paste combination pressed again straight away, with nothing
in between, takes it back.</p>
<h3>Things that destroy work</h3>
<p>Loading, saving over, deleting and the like sit on a knob and need four clicks clockwise, shown as {{>>>}} filling
up. Any other knob, or a click back, cancels. Nothing destructive is ever on a pad. Clearing or pasting over a whole
pattern needs its button held for half a second, and whatever the edit combinations do can be undone.</p>
<h3>Page keys</h3>
<p>On screens with several pages, [[Func]] + a top-row pad jumps straight to a page (on {{PARAM}}, to a section; the
same pad again steps within it).</p>
<h3>Pinning the playing pads</h3>
<p>On {{DRUMS}} or {{SCALE}}, hold [[Shift]] and hold that screen's own bottom pad for half a second. {{PIN}} appears
in the name bar and those pads keep playing, with pressure, on every other screen, so you can play pads with one hand
and turn sound knobs with the other. The same gesture unpins.</p>""")),
    ]))

    # =========================================================================================
    scr = []
    scr.append((None, fmt("<p>Every screen, what its knobs show and what its pads do. Short names are explained in Appendix A.</p>")))

    def screen(title, shots, text):
        figs = '<div class="shots">' + "".join(S.svg(n, p, c) for n, p, c in shots) + "</div>"
        scr.append((title, figs + fmt(text)))

    screen("TRACKS — the mixer", [("TRACKS", 1, "VOL"), ("TRACKS", 5, "ENG")], """
<p><b>Knobs:</b> knob N belongs to track N. Five pages: {{VOL}}, {{PAN}}, {{SND1}}, {{SND2}}, {{ENG}}.</p>
<p><b>Pads:</b> the top row selects a track. The bottom row mutes (green = sounding, red = muted, blue = solo);
[[Func]] + bottom pad solos.</p>
<p><b>Changing a track's engine:</b> on the {{ENG}} page turn the track's knob to choose an engine. It is shown boxed
as pending and nothing changes yet. Tap that track's top pad (lit amber) to commit. Leaving the page drops the choice.
Changing engine replaces the track's sound in every pattern.</p>""")
    screen("STEP — the step sequencer", [("STEP", 1, "STEP")], """
<p><b>Knobs:</b> {{LN}} track length, {{SP}} track speed, {{QN}} playback quantize, and the tempo on knob 8.</p>
<p><b>Pads:</b> sixteen steps. A tap adds a step; a quick tap on a lit step removes it; a held step is never
removed. [[▲]] / [[▼]] move to the next sixteen steps of a longer track. Hold a step and press a key to set its note
(several keys for a chord).</p>
<p><b>Step detail:</b> hold a step (a little longer than a tap) and the screen changes to that step's own pages,
without anything else being touched. It opens on the track's {{MAIN}} page (a drum track: the {{DR}} page of the
drum that step plays; a MIDI track: its first page), and every cell shows the value that step will play: a small
mark in the cell's corner means the step has its own value there (a lock), otherwise it is the track's value. Every
turn is a lock. The knob-page buttons move through all the track's sound pages while the step is held, and one
press back from {{MAIN}} is {{TRIG}} ({{NOTE VEL LEN MICR}} | {{COND PROB A B LCKS}}), the step itself. The name bar
shows the step ({{S5 LOCK}}). Let go and {{STEP}} is back. Turning a knob straight after pressing opens the pages at
once; a quick tap never shows them. With several steps held, the pages belong to the one nearest the top left.</p>
<p><b>Recorded pressure:</b> a step whose notes were recorded from pads with pressure has a small dark dot at the
lower right of its cell, and one more page, {{PRES}}, one press back from {{TRIG}} (the pages go round, so it is
also the last page). It shows the recorded curves over the note's length and three cells: {{PRES}} sets how much of
the pressure is played (0 to 200 %; 0 plays none and keeps the recording), {{>LCK}} turns it into parameter locks,
{{CLR}} removes it. {{>LCK}} and {{CLR}} need four clicks clockwise.</p>""")
    screen("PARAM — the sound", [("PARAM synth", 1, "Synth, MAIN (the landing page)"), ("PARAM synth", 7, "Synth, FILTR"), ("PARAM FM", 2, "FM, FMMIX")], """
<p><b>Knobs:</b> the pages of the selected track's engine (Chapter 5), opening on {{MAIN}}: cutoff, resonance and
the level envelope, the same on every engine that has a filter. A drum track shows one {{DR}} page for the pad last
selected on {{DRUMS}} (and its {{KICK}} page when that pad is a kick machine), then its LFOs.</p>
<p><b>Pads:</b> the track's steps, for locks: dim = empty, green = has a note, orange = has locks, white = held. A
bottom-row pad under a knob that has a pressure range is violet and plays that range instead.</p>""")
    screen("TRK FX — track effects", [("TRK FX", 1, "TRACK"), ("TRK FX", 6, "FX")], """
<p>The same six pages on every track, each with a page key ([[Func]] + top pad 1 to 6):</p>
<ul>
<li>{{TRACK}}: {{VOL PAN SND1 SND2}} | {{BUS1 BUS2 LEN SPD}}. The send cells are labelled by what the bus carries
({{RVB1}}, {{DLY2}}).</li>
<li>{{ARPLK}}: {{GATE VEL RTCH}}, there to be locked per step so the arp changes along the pattern.</li>
<li>{{DIRT}}, {{TALK}}, {{MODFX}}: the three character blocks (Chapter 8).</li>
<li>{{FX}}: {{SLOT TYPE ON WET}} and the chosen insert's four controls.</li>
</ul>
<p><b>Pads:</b> as on {{PARAM}}; every cell here can be locked and given a pressure range.</p>""")
    screen("DRUMS — drum pads", [("DRUMS", 1, "DRUMS")], """
<p><b>Pads:</b> sixteen drum voices, played with velocity and pressure. One colour per kind of voice; white while it
sounds. Playing a pad selects it.</p>
<p><b>Knobs:</b> the selected pad's voice: {{TN DC TO DV SN LV PN TY}} (tune, decay, tone, drive, snap, level, pan,
type). The screen follows the selected track if it is a drum track, otherwise the first drum track.</p>""")
    screen("PRESET — sound presets", [("PRESET", 1, "PRESET")], """
<p><b>Knobs:</b> {{PS}} scroll, {{CT}} category, {{EN}} this engine or any, {{AU}} audition, {{LD}} load, {{SV}}
save, {{DL}} delete, {{VL}} track volume. Load, save and delete need the four clicks.</p>
<p><b>Audition:</b> opening the screen changes nothing. As soon as the highlight moves (knob 1, [[▲]] / [[▼]], or a
change of category or engine filter), the highlighted preset takes over the track's sound for listening only, and
the name bar shows AUD. The track's own sound comes back when you leave the screen by any route or select another
track. {{LD}} keeps the highlighted preset instead (the name bar shows LOAD OK): after that nothing is put back.
{{AU}} is the off switch: turned off (AU- in the name bar), the list scrolls without changing the sound, and
{{LD}} still loads. A project saved while you are listening stores the track's own sound.</p>
<p><b>Pads:</b> play the track, sixteen semitones up from middle C, so you can audition without reaching for the keys.
[[▲]] / [[▼]] also scroll the list.</p>
<p>Your own presets are files in <i>Music / Neptune / Presets</i>.</p>""")
    screen("SETTINGS", [("SETTINGS", 1, "SETUP"), ("SETTINGS", 2, "MASTR"), ("SETTINGS", 3, "PROJ")], """
<p>Three pages. {{SETUP}}: tempo, swing, quantize, clock source, master volume, pattern, and {{HELP}} (when the
help cards appear). {{MASTR}}: master EQ,
compressor and limiter. {{PROJ}}: project slots (Chapter 12).</p>""")
    screen("FLTR — the filter picture", [("FLTR", 1, "FLTR")], """
<p>The filter's actual response, measured from the filter itself, with the engine's filter controls beneath as
two-letter labels. For the synth's two filters it shows them combined as routed. Further filter pages of the engine
follow on the knob-page buttons. On the filter envelope's page, turning an envelope knob shows the envelope's shape
in place of the response for a second. Drums and MIDI tracks have no filter.</p>""")
    screen("ARP — arpeggiator and rolls", [("ARP", 1, "ARP"), ("ARP", 3, "VARY"), ("ARP", 6, "ROLL")], """
<p>Seven pages for the selected track's arpeggiator: {{ARP}}, {{PLAY}}, {{VARY}}, {{MUTA}}, {{ACFG}}, and {{ROLL}} /
{{ROL2}} for pad rolls (Chapter 10). An {{ARP}} tag in the name bar shows the arp is on.</p>""")
    screen("SCALE — scale pads", [("SCALE", 1, "SCALE")], """
<p><b>Pads:</b> play the selected track in a scale, with pressure; the root is blue, other notes green. [[▲]] /
[[▼]] change the scale page. Twelve pages are built in (chromatic, major, minor, pentatonics, blues, whole-tone and
modes) and you can make your own.</p>
<p><b>Knobs:</b> {{PG}} page, {{RT}} root, {{MD}} relative or absolute, {{ED}} edit, {{KS}} and {{KM}} let the keys
choose pages, {{OC}} octave, {{LK}} scale lock (the same switch as the [[Scale]] button: with the lock on, the keys
play the nearest note of the page's scale on synth tracks, arps stay in it, and <code>SCALE</code> shows in the name
bar). In edit mode tap a pad and press a key to assign its note.</p>""")
    screen("SONG", [("SONG", 1, "SONG")], """
<p><b>Knobs:</b> {{PA}} pattern, {{RP}} repeats, {{TR}} transpose, {{RW}} row, {{LP}} loop, {{SG}} song on / off,
{{IN}} insert a row, {{DL}} delete the row (four clicks).</p>
<p><b>Pads:</b> pad N selects row N; the pad after the last row adds one. With {{SG}} on, [[Play]] follows the rows;
a row ends when track 1 has wrapped the set number of times.</p>""")
    screen("AT ROUTE — pad pressure", [("AT ROUTE", 1, "AT ROUTE")], """
<p>What pad pressure does on the selected track. Top row: the destination of routes 1 to 4. Bottom row: how far each
moves. Every track has its own routes; the destinations on offer depend on the engine (Chapter 7). The pads play the
track chromatically so you can feel the result.</p>""")
    screen("WAVE — waveform and resampling", [("WAVE", 1, "WAVE"), ("WAVE", 2, "REC")], """
<p>{{WAVE}} page: the track's sample with start and end brackets, loop marks, slice ticks and a moving line for each
sounding voice (in granular mode, a tick per grain). Knobs: {{ST EN LS LE}} markers, {{ZM}} zoom, {{SC}} scroll,
{{MD}} play mode, {{SL}} slices. Pad N selects slice N.</p>
<p>{{REC}} page: resampling (Chapter 11).</p>""")
    ch.append(("The screens", scr))

    # =========================================================================================
    eng = []
    eng.append((None, fmt("""
<p>Each track has one engine, chosen on {{TRACKS}} → {{ENG}}. Its pages appear on {{PARAM}}. All engines except
drums and MIDI have a filter (Chapter 6); all sound engines have LFOs (Chapter 7).</p>
<h3>The landing page: MAIN</h3>
<p>{{PARAM}} opens on the same page for every engine that has a filter, so the first handles of a sound are always
under the same knobs: knob 1 is the filter cutoff ({{CUT}}), knob 2 the resonance ({{RESO}}), knob 3 how far the
filter envelope moves the cutoff ({{ENV}}; on FM, macro, wavetable, wave sequencer and synth; the sampler shows
{{ENVF}}), knob 4 the filter {{TYPE}}, and knobs 5 to 8 are the envelope that shapes the level: {{ATK}}, {{DEC}},
{{SUS}}, {{REL}} (on FM, operator 1's). These are the same cells as on the engine's {{FILTR}} and {{AMP}} pages
further on: a value, a parameter lock or a pressure range set here is set there too. Turning them shows the filter
or envelope picture. The engine's own pages follow, in the tables below.</p>
<h3>How the engines relate</h3>
<p>The engines are separate instruments, one per track. The synth is the one that mixes kinds of oscillator: each
of its three oscillators can be an analog wave, a wavetable, an FM pair, a formant oscillator or a harmonic
oscillator, in any combination, into the same mixer, filters and envelopes (see the synth, below). The FM, macro,
wavetable and wave-sequencer engines go deeper into one idea each (six operators, physical models, timed scans
through a recording, sequenced waves) and are chosen per track. {{ARCH}} (mono, hybrid, poly) only loads different
starting values into the synth's own pages. Every engine shares what comes after the oscillators: the same five
filter models, the same envelopes and LFOs, the same effects and the same sequencer.</p>
<p>There is also a bridge between engines: resampling. Any track (a macro oscillator, an FM sound, the synth) can be
recorded on the {{WAVE}} screen's {{REC}} page, and the recording becomes a sample for a sampler track or the
{{USER}} table of a wavetable track (Chapter 11). That is how a macro-oscillator sound becomes a wavetable. To
layer two engines on one note, put them on two tracks and play or sequence both.</p>""")))
    eng.append(("Synth — the configurable voice", fmt("""
<p>Three oscillators and noise into two filters and an amplifier, up to sixteen voices. {{ARCH}} on the {{VOICE}}
page loads a starting point (mono, hybrid, poly) into the same pages; {{MODE}} sets mono, duo, poly or unison;
{{STCK}} stacks detuned copies. {{PW}} shapes every waveform. At 50 each wave is plain, and the two
directions are two different sounds that grow smoothly out of it:</p>""") + table(["Wave", "Below 50: thinner, sharper", "Above 50: thicker"], [
        ("<code>SQR</code> <code>PLS</code>", "The pulse narrows: classic pulse width, thin and nasal at the bottom.", "A square an octave higher is mixed in, up to half and half."),
        ("<code>SIN</code> <code>TRI</code>", "Wavefolding: the wave is driven up to six times too hard and the excess folded back, for bright, glassy harmonics.", "Saturation: the wave is pushed into a limiter and squares off, warm then hollow."),
        ("<code>SAW</code>", "A second saw is added, a part of a cycle behind the first: the doubled, phasing saw of saw PWM.", "A saw half a cycle behind is taken away: the saw hollows out into a square."),
    ], "cols3") + fmt("""
<p>An LFO sent to {{PW}} animates whichever wave is selected; centred on 50 it sweeps through both sounds.</p>
<h3>Oscillator kinds: wavetable, FM, formant, harmonic</h3>
<p>{{WAV1}}, {{WAV2}} and {{WAV3}} go on past the analog waves to four other kinds of oscillator: {{WT}}, {{FM}},
{{FORM}} and {{HARM}}. Any slot can be any kind, so a wavetable can sit beside a saw and an FM pair, all three
through the same filter. Tuning, detune, cross-modulation, the stack and the mixer treat every kind alike. Each
non-analog slot has two controls of its own on the {{SRC}} page, labelled with the slot's number (slot 1 on knobs
1–2, slot 2 on 3–4, slot 3 on 5–6; a slot playing an analog wave shows nothing there):</p>""")
    + table(["Kind", "First control", "Second control", "What it is"], [
        ("<code>WT</code>", "<code>POS</code> position in the table", "<code>TBL</code> table: SAW SQR TRI SIN PWM FORM HARM SPEC DIGI", "A wavetable oscillator reading one of the nine built-in tables; sweep POS for movement."),
        ("<code>FM</code>", "<code>IDX</code> modulation depth", "<code>RAT</code> modulator ratio, 1–12", "A sine modulated by a second sine at a whole multiple of its pitch: bells, basses, glassy tones."),
        ("<code>FORM</code>", "<code>FRM</code> formant frequency, 1–16 times the pitch", "<code>WID</code> width of the formant", "A resonant, vocal peak that stays put while the note changes character around it."),
        ("<code>HARM</code>", "<code>TLT</code> tilt, bright to dark", "<code>ODD</code> all harmonics to odd only", "Up to twelve harmonics added together; odd-only is hollow like a square."),
    ]) + fmt("""
<p>Whatever moves {{PW}} (an LFO with {{DEST}} {{PW}}, pad pressure routed to {{PW}}) also moves the first control
of every non-analog slot, so one LFO can sweep a pulse's width and a wavetable's position together. Choosing a new
kind for a slot starts it from that kind's own settings. The wavetable kind plays the built-in tables; a table
made from your own recording ({{USER}}) is in the wavetable engine.</p>""") + page_list(params, "Unified Voice") + fmt("""
<p>Three more pages follow for modulation: {{MOD}} and {{MOD2}} (up to four LFOs) and {{MENV}} (up to four extra
envelopes). See Chapter 7.</p>
<p>The {{ANALG}} page is the instrument's "age" control: drift, tuning slop, and per-note variation of waveform,
filter and envelope. With everything at zero, successive notes are identical; raise them and no two are.</p>""")))
    eng.append(("FM — six operators", fmt("""
<p>Six sine operators, eight algorithms. {{FMMIX}} puts all six operator levels under knobs 1 to 6 so you can play
the timbre directly; {{RATIO}} does the same for their frequency ratios. Each operator then has its own page with
ratio, level, detune, feedback and envelope.</p>""") + page_list(params, "FM Synth")
        + "<p>Algorithms: " + " · ".join(f"<code>{html.escape(c)}</code>" for c in choices(params, "FM Synth", "FMMIX", "ALGO")) + ".</p>"))
    eng.append(("Macro — eight models, four knobs", fmt("""
<p>One oscillator that can be eight different things. {{MODL}} chooses the model, and knobs 2 to 5 are renamed to
suit it.</p>""") + table(["Model", "What it is", "Knobs 2–5"],
                          [(f"<code>{m}</code>", d, " ".join(f"<code>{x}</code>" for x in k.split())) for m, d, k in ctx["macro_models"]])
        + page_list(params, "Macro Oscillator")))
    eng.append(("Wavetable", fmt("""
<p>{{TABL}} picks one of nine built-in tables or {{USER}}, a table built from any sample (64 frames, so {{POS}} scans
through the recording). {{MSPD}} and {{MDPT}} move the position by themselves; {{ENVP}} and {{VELP}} move it with
the envelope and velocity. {{INTP}} sets how frames blend, from smooth to deliberately coarse.</p>
<p>The {{SCAN}} page moves the position by itself over a set time, whatever note you play: this is time stretch
for a wavetable. {{MODE}} runs once and holds ({{RUN}}), loops, goes there and back ({{PING}}), moves in audible
steps ({{STEP}}) or jumps to random frames ({{RAND}}). The pass lasts {{TIME}} seconds, or with {{SYNC}} on, {{LEN}}
at the tempo; with {{SYNC}} = {{NAT}} a USER table plays back at the original recording's length, and {{SPD}} then
slows it or speeds it without changing pitch. The scan starts from {{POS}}.</p>
<p>{{FRMS}} chooses how a recording is cut into the USER table. The {{A64}}, {{A128}} and {{A256}} settings align
each frame to the sound's pitch period: scanning is far smoother and the table plays in tune with the keys.
They work on pitched material; noise and silence are cut the plain way.</p>""")
        + page_list(params, "Wavetable")
        + "<p>Tables: " + " · ".join(f"<code>{c}</code>" for c in choices(params, "Wavetable", "WAVE", "TABL")) + ".</p>"))
    eng.append(("Wave sequencer", fmt("""
<p>Each note steps through a short sequence of waves. Pitch, shape, level and step time each have their own lane of
up to sixteen steps, and the lanes can be different lengths ({{TLEN PLEN SLEN LLEN}}), so the combination drifts
before repeating. On the lane pages, knob N is step N.</p>""") + page_list(params, "Wave Sequencer")))
    eng.append(("Sampler", fmt("""
<p>Plays a sample from the bank: the built-in loop and hits, WAV or AIFF files you put in <i>Music / Neptune /
Samples</i>, and anything you resample. {{MODE}} sets how it plays; {{GRAN}} switches to granular playback, with its
own page. The {{SLICE}} page cuts the sample into slices, evenly or at its hits, and the {{STRCH}} page changes its
speed without changing its pitch (all in Chapter 11). Edit the markers by eye on the {{WAVE}} screen.</p>""") + page_list(params, "Sampler")))
    eng.append(("Drums", fmt("""
<p>Sixteen voices, each a small synthesizer with eight controls. Play and select them on {{DRUMS}}; edit the selected
one there or on {{PARAM}}. Closed and open hats cut each other off.</p>""") + page_list(params, "Drum")
        + "<p>Types: " + " · ".join(f"<code>{c}</code>" for c in choices(params, "Drum", "DR 1", "TYPE"))
        + fmt(""" (FM kick, analog kick, modelled kick, noise snare, FM snare, clap, closed hat, open hat, cymbal, tom,
rim, shaker, clave, sub-bass, kick machine).</p>
<p>The five sound cells act on every type. {{DEC}} is the length: a drum is a one-shot and rings for its {{DEC}}
however short the pad press or the step ({{SUB}}, the bass note, is the exception: it sounds while held and fades
over {{DEC}} afterwards). {{TONE}} is brightness in the way of each drum (Appendix A lists them). {{DRIV}} is
saturation. {{SNAP}} is the front edge: 50 leaves the drum as it is, lower softens the onset, higher lifts it and
gives the tonal drums a click. Pad pressure ({{AT ROUTE}}) reaches {{DEC}}, {{TUNE}}, {{TONE}}, {{LEVL}}, {{DRIV}}
and {{PAN}} of the hit that is sounding; {{SNAP}} belongs to the strike, so pressure cannot change it.</p>
<h3>The kick machine (KSYN)</h3>
<p>The lower left pad of a new kit is a kick machine; set any other pad's {{TYPE}} to {{KSYN}} and it becomes one
too. A kick machine is a full kick synthesizer with a second page, {{KICK}}: on
{{DRUMS}} press a knob-page button (the labels become {{SW ST SC SH KN KT HD AC}}); on {{PARAM}} it is the page after
the pad's own.</p>
<ul>
<li>On the pad's first page: {{TUNE}} is the pitch the drum settles on, {{DEC}} how long the body rings, {{TONE}} a
low-pass over the whole drum, {{DRIV}} saturation, {{SNAP}} how hard the front edge is.</li>
<li>{{SWEP}}: how far above {{TUNE}} the pitch starts, in semitones (0 = no sweep, 72 = six octaves).
{{SWPT}}: how long the fall takes, 2 ms to 1 s. {{SCRV}}: where in that time the fall happens (negative: nearly all at
the start, the classic kick; positive: late).</li>
<li>{{SHAP}}: the body's waveform, from a sine through a triangle to a squared-off wave.</li>
<li>{{KNCK}}: the level of the knock, a short tick at the very start. {{KTON}}: its tone, from a dull thud that lasts
about 12 ms to a bright tick of about 1.5 ms.</li>
<li>{{HOLD}}: time at full level before the decay starts, up to 0.3 s, for long, flat 808-style bodies.
{{ACRV}}: the shape of the decay.</li>
</ul>
<p>Every one of these can be locked per step and given a pressure range, like any other cell. The other fourteen
types are unchanged and ignore the {{KICK}} page.</p>
<h3>Playing the kit</h3>
<p>The pads play the kit on {{DRUMS}} ([[Shift]] + bottom pad 2); on the other screens the pads do that screen's own
job, unless {{DRUMS}} is pinned. The keys play the kit on every screen while a drum track is selected. From the
lowest C, eight keys (C to G) are the bottom pad row from left to right, so the lowest C is the lower left pad; the
next eight (G♯ to D♯) are the top row; the last eight (E to B) are the bottom row once more, and the top C is the
lower left pad again. Harder keys are louder. The layout repeats every two octaves, so after one press of an octave
button the same rows sit an octave along, and after two the keys are as they were.</p>
<div class="note"><b>Sequencing drums</b>Record drum patterns by playing the pads or the keys with [[Rec]] on. A
step tapped in on {{STEP}} plays the pad last selected on {{DRUMS}}; hold the step and press a key to give it that
key's drum. The steps can be removed, locked and given conditions on {{STEP}} like any others.</div>""")))
    eng.append(("MIDI track", fmt("""
<p>Sends notes, pitch bend, per-note pressure, program changes and controllers to outside gear. Its pages send
standard controllers, so the same knobs, locks and pressure ranges work on a hardware synth.</p>""")
        + page_list(params, "MIDI Track")))
    ch.append(("Sound engines", eng))

    # =========================================================================================
    ch.append(("Filters", [
        (None, fmt("""
<p>Every filter cell uses the same words: {{TYPE}} is the model, {{MODE}} its response, then {{CUT}} and {{RESO}}.
While you turn any filter knob, the response curve is drawn over the half of the grid you are not using; {{FLTR}}
shows it full-size.</p>
<p><b>Envelopes have a picture too.</b> On any page with an envelope ({{AMP}}, {{F ENV}}, {{MENV}}, an FM operator's
page), turning {{ATK}}, {{DEC}}, {{SUS}}, {{REL}} or {{CURV}} draws the envelope's shape over the other half of the
grid for a second: the rise, the fall to the sustain level, a held stretch, and the release, with dotted lines where
one stage ends and the next begins. {{CURV}} bends the lines exactly as it bends the sound. Each stage is drawn wider
the longer it lasts, but on a compressed scale (a stage ten times longer is a few columns wider, not ten times), so a
1 ms attack and a 10 s release can share the picture.</p>
<p><b>Each stage can be bent on its own.</b> Hold [[Shift]] and turn {{ATK}}, {{DEC}} or {{REL}}: instead of a fine
change of the time, that one stage changes shape. Turned left (down to −100) the move comes early: an attack that
bulges upward, a decay or release that drops at once and then levels out. Turned right (up to +100) it comes late: a
hollow attack, a decay or release that holds and then falls. 0 is a straight line. While you turn, the cell shows the
curve ({{ACRV}}, {{DCRV}} or {{RCRV}}), the name bar says for example "ATK CRV +40", and the picture shows the new
shape; a second later the cell shows the time again. This works wherever those three cells belong to an envelope:
{{MAIN}}, {{AMP}}, {{F ENV}}, {{MENV}}, every FM operator, on {{FLTR}}, and on a held step, where it locks the curve
for that step. {{SUS}} is a level and has no shape, so [[Shift]] stays fine there; a MIDI track's {{ATK}}, {{DEC}}
and {{REL}} are controller values and have no curve either. Where there is a {{CURV}} cell it still bends all three
stages together, and each stage's own bend is added to it. The curves are saved with the sound.</p>""")
            + table(["TYPE", "What it is", "MODE", "Character"], [
                ("<code>SVF</code>", "12 dB state-variable", "LP BP HP NTCH", "Clean, stable right up to the top; the neutral choice."),
                ("<code>SEM</code>", "Expander-style 12 dB", "MRPH BP", fmt("Gentle and wide. In MRPH, {{MORP}} sweeps low-pass through notch to high-pass. Never self-oscillates.")),
                ("<code>LADR</code>", "Transistor ladder", "LP24 LP18 LP12 LP6 BP HP", "The classic thick low-pass. Self-oscillates at full resonance."),
                ("<code>DIOD</code>", "Two-pole with diode limiting", "LP HP BP", "Rougher, asymmetric resonance."),
                ("<code>COMB</code>", "Tuned feedback comb", "POS NEG", "Metallic, pitched resonances; cutoff sets the pitch."),
                ("<code>OFF</code>", "Bypassed", "—", "—")])
            + fmt("""
<p>{{DRIV}} overdrives the filter input, each type in its own way (the SVF soft-clips; on COMB it darkens the
feedback). {{MORP}} acts in one place, type SEM in mode MRPH; anywhere else the knob says {{SEM MRPH ONLY}}. {{ENV}} sets how far the filter envelope moves the
cutoff, up to seven octaves either way. {{KTRK}} / {{KEY}} makes the cutoff follow the keyboard.</p>
<p><b>The synth has two filters.</b> The {{FLT B}} page sets the second one: its own type and mode, its cutoff as an
offset from filter A ({{OFSB}}), and {{ROUT}}: single, series or parallel, with {{BAL}} to balance them.</p>
<p><b>FM, macro, wavetable and wave sequencer</b> share one filter design ({{FILTR}} and {{F ENV}} pages), open by
default so presets sound as they did before you touch it. <b>The sampler</b> has its own {{FILTR}} page.
A filter is also available as an insert effect ({{FLT}}) for any track, drums included.</p>""")
            + '<div class="shots">' + S.svg("FLTR", 1, "FLTR") + S.svg("PARAM synth", 8, "Synth, FLT B") + "</div>")]))

    # =========================================================================================
    lfo_shapes = choices(params, "FM Synth", "LFO1", "SHPE")
    dests = []
    for e_name, shown in (("FM Synth", "FM"), ("Macro Oscillator", "Macro"), ("Wavetable", "Wavetable"),
                          ("Wave Sequencer", "Wave seq"), ("Sampler", "Sampler"), ("Drum", "Drums")):
        dests.append((shown, dest_cell(choices(params, e_name, "LFO1", "DEST")[1:])))
    ch.append(("Modulation and pressure", [
        ("LFOs", fmt("""
<p>LFOs run per voice: each note has its own. The synth has up to four, on its {{MOD}} and {{MOD2}} pages (choose
which with {{LFO}}, how many with {{LFOS}}). Every other sound engine ends with three pages: {{STRIP}} (what the mod
strip does, see Strips below), then {{LFO1}} and {{LFO2}}.</p>""")
            + table(["Cell", "What it does"], [
                ("<code>RATE</code>", fmt("Speed in Hz. With {{SYNC}} on, a tempo division instead (8 bars to 1/32, dotted and triplet included); both values are remembered.")),
                ("<code>DPTH</code>", "Amount; negative inverts."),
                ("<code>SHPE</code>", " ".join(f"<code>{s}</code>" for s in lfo_shapes) + " — sine, triangle, square, saw, stepped random, smooth random, ramp, and two exponential shapes that only push one way."),
                ("<code>TRIG</code>", fmt("{{FREE}} runs on regardless of notes. {{KEY}} restarts with each note. {{ONE}} plays one cycle and stops: a one-shot envelope in any shape. {{HALF}} plays half a cycle. {{HOLD}} takes one value at note-on and keeps it: a different value per note.")),
                ("<code>DEST</code>", "What it moves; see below."),
                ("<code>FADE</code>", "Fade-in time after the note starts."),
                ("<code>PHAS</code> <code>DLY</code>", "Synth only: start phase, and a wait before the LFO begins."),
                ("<code>WHL</code> <code>VEL</code> <code>PRES</code>", fmt("Synth only: the mod strip, velocity, and the note's own pad pressure add to the depth. {{DPTH}} 0 with {{WHL}} up gives vibrato from the strip alone.")),
                ("<code>KTRK</code>", "Synth only: higher notes speed the LFO up.")])
            + "<h3>Destinations</h3>"
            + fmt("""
<p>An LFO can reach nearly everything its engine has a knob for. {{DEST}} starts with the engine's basic
destinations (pitch, level, pan, cutoff, resonance and the engine's main controls) and goes on with every other
continuous cell of the engine's pages, page by page; on the six engines with two LFOs the list ends with the other
LFO's rate and depth ({{LRAT}}, {{LDPT}}). Plain turns go one entry at a time; [[Func]] + turn jumps to the start of
the next page's group, so the far end of a long list is a few clicks away.</p>
<p>A destination that is a page cell has that cell's name, or the name with a number or an F in front where two
pages use the same word ({{ATK1}} is operator 1's attack, {{FATK}} the filter envelope's). The LFO moves the cell
around the value its knob is set to, in the cell's own scale: at {{DPTH}} 100 a 0–100 cell swings by its whole range
and a time or frequency cell by half its range in octaves, never past the cell's limits. Turn the knob and the swing
moves with it. Every note has its own LFOs, so in a chord each note's cell moves by itself.</p>""")
            + table(["Engine", "DEST offers (after OFF)"], [("Synth", dest_cell(SYNTH_DESTS.split()))] + dests)
            + fmt("""
<p>On the synth, {{1A}} to {{3B}} are the two {{SRC}} cells of each oscillator slot; {{ATK2}} to {{REL5}} and
{{EDPT}} are the extra envelopes of the {{MENV}} page. On the wave sequencer, {{P1}}–{{P16}}, {{L1}}–{{L16}} and
{{T1}}–{{T16}} are the pitch, level and time of one step.</p>
<div class="note"><b>Struck values</b>Some values are read once, when the note is struck, so the LFO sets them per
note rather than moving them while the note sounds. Use {{FREE}} or {{HOLD}} there; {{KEY}} would give the same
value every time. Drums: {{DEC}}, {{SNAP}}, and on a kick-machine pad {{SWPT}}, {{SCRV}}, {{KNCK}}, {{HOLD}},
{{ACRV}}. Sampler: {{STRT}}, {{END}}, {{LPST}}, {{LPEN}}, {{SLIC}}, {{FADE}}, {{SLEN}}, {{VELF}}, {{KTRK}},
{{ATK}}, {{DEC}}, {{SUS}}, {{REL}}, {{RAND}}; its grain cells {{SPRY}}, {{PJIT}}, {{SPRD}}, {{RVRS}} are read
each time a grain starts.</div>
<div class="note"><b>Not in the lists</b>Cells that choose from a list or count something (types, modes, voices,
steps), cells that rebuild a sample or a table, whole-sound tuning ({{PTCH}} is the pitch destination), envelope
curves, and {{LEVL}} where it is the level of the mixed voices (synth, FM, wavetable: use {{AMP}}).</div>""")),
        ("Extra envelopes", fmt("""
<p>The synth's {{MENV}} page adds up to four envelopes beyond the amp and filter ones: {{ENV}} selects, {{ENVS}} sets
how many run, then {{ATK DEC}} | {{SUS REL DPTH DEST}}, with destinations amp, cutoff or pitch and then the same long list as the LFOs.</p>""")),
        ("Pad pressure", fmt("""
<p>Each pad sends its own pressure, and it reaches only the note that pad is playing. There are three ways to use it.</p>
<h3>1. Routes, per track</h3>
<p>{{AT ROUTE}} gives each track up to four routes: a destination and a depth. A new track starts with pressure on
level (drums: decay).</p>""")
            + table(["Engine", "Pressure destinations"], [
                ("Synth", "LEVL CUT VIB PW MIX XMOD"), ("FM", "LEVL CUT FM"), ("Macro", "LEVL CUT VIB TIMB MORP"),
                ("Wavetable", "LEVL CUT VIBR POS"), ("Wave seq", "LEVL CUT"),
                ("Drums", "DEC TUNE TONE LEVL DRIV SNAP PAN (one route for the kit)"), ("Sampler", "none (use ranges, below)"),
                ("MIDI track", "sends per-note pressure out")])
            + fmt("""
<h3>2. Ranges, on any knob</h3>
<p>On {{PARAM}}, {{TRK FX}} or {{FLTR}}: turn a knob to a value, then [[Func]] + [[▲]] sets that as the high end of a
pressure range, or [[Func]] + [[▼]] as the low end. The name bar confirms ({{RESO HI 80}}). The bottom-row pad under
that knob turns violet; pressing into it sweeps the knob and lets it return on release. With one end set, pressure
moves from the knob's value to that end; with both, from low to high. The same gesture again clears the end.</p>
<h3>3. The arp and rolls</h3>
<p>Pressure can drive the arp's rate, octave range or gate ({{ACFG}} page) and the speed of pad rolls ({{ROLL}}).</p>""")),
        ("Strips", fmt("""
<p>The pitch strip bends the selected track by its engine's {{BEND}} range (a drum kit: two semitones, the whole
kit together) and springs back. It acts on the selected track only: other tracks playing from the sequencer stay
in tune.</p>
<p>The mod strip holds its position. It always does something to the selected track, on every engine and every
factory preset, and nothing at all when it is at the bottom. What it does is yours to change. On FM, macro,
wavetable, wave sequencer, sampler and drums the {{STRIP}} page (just before {{LFO1}}) has three routes,
{{DST1}} {{AMT1}}, {{DST2}} {{AMT2}}, {{DST3}} {{AMT3}}: each sends the strip to one destination of the list the LFOs
use (Func + turn jumps from group to group) by a signed amount in that destination's own scale, so +100 with the
strip fully up is where an LFO at full depth would peak. Pitch goes by the amount squared, up to an octave. Routes to
the same destination add up, {{LRAT}} and {{LDPT}} from the strip act on both LFOs, and an amount of 0 or a destination
of {{OFF}} switches a route off. {{WVIB}} is vibrato the strip brings in (semitones at the top of the strip) and
{{VRAT}} its speed. The amounts can be locked per step. Until you change the page, the strip does this:</p>""")
            + table(["Engine", "The mod strip by default"], [
                ("Synth", fmt("Goes to every LFO whose {{WHL}} is set ({{MOD2}} page), adding to that LFO's depth. While no LFO in use has a {{WHL}} amount it is vibrato, up to half a semitone. An LFO set to {{EXPU}}, {{ONE}}, 40 Hz, {{DPTH}} 0 passes the strip straight on to its {{DEST}}: that is how the factory presets and the three {{ARCH}} sounds open a filter or raise cross-mod from the strip.")),
                ("FM", fmt("{{DST1}} {{INDX}} +100: raises the modulation index, up to three times (brighter, harder; from the strip {{INDX}} counts double). {{WVIB}} 0.25: a little vibrato. It also moves the cutoff by {{WHL}} on the {{F ENV}} page, as on the next three engines.")),
                ("Macro", fmt("{{DST1}} the fourth macro ({{MORP}} in most models) +100: sweeps it to its top. {{DST2}} the first macro +30. {{WVIB}} 0.25. Cutoff by {{WHL}}.")),
                ("Wavetable", fmt("{{DST1}} {{POS}} +60: scans the table forward from {{POS}}, up to 60 % of the table. {{WVIB}} 0.25. Cutoff by {{WHL}}.")),
                ("Wave sequencer", fmt("{{DST1}} {{RATE}} +50: runs the sequence faster, twice as fast at the top (so the top of the strip is in time again). Cutoff by {{WHL}}.")),
                ("Sampler", fmt("Wears the sound down: {{DST1}} {{DRIV}} +70, {{DST2}} {{RATE}} +30 (sample-rate reduction, {{TONE}} page), {{DST3}} {{SPRY}} +50 (scatters the grains in {{GRAN}} mode). Whatever the strip adds to {{DRIV}} is mostly taken out of the level again, so the tone changes more than the loudness.")),
                ("Drums", fmt("Opens the whole kit up: {{DST1}} {{DEC}} +79, every drum rings up to three times as long (from its next hit), and {{DST2}} {{DRIV}} +50, driven harder. The routes belong to the kit: every pad follows them.")),
                ("MIDI", "Sends controller 1 to the instrument outside."),
            ])
            + fmt("""
<p>The strip moves the selected track only, and every track keeps the value the strip last gave it. Set one track's
strip, select another and move the strip there: the first stays as you left it. Selecting a track changes nothing
until you touch the strip, and a preset you load on a track is given that track's value again. While a track's arp is on, the strip can drive one arp macro instead ({{STRP}} on the
arp's {{VARY}} page); the track's sound then keeps whatever the strip last gave it.</p>""")),
    ]))

    # =========================================================================================
    fx_rows = [(f"<code>{f['name']}</code>", ctx["fx_names"][f["name"]],
                " ".join(f"<code>{html.escape(s['label'])}</code>" for s in f["specs"] if s["id"] >= 0)) for f in params["effects"]]
    ch.append(("Effects", [
        ("Signal path", fmt("""
<p>Engine → {{DIRT}} → {{TALK}} → {{MODFX}} → insert slots → mixer (volume, pan) → master EQ, compressor, limiter.
Each track also sends to two shared buses. All of a track's effect pages are on {{TRK FX}}.</p>""")),
        ("The three character blocks", fmt("""
<p>On every track, always available, bypassed while at zero.</p>
<ul>
<li>{{DIRT}}: {{DRIV TYPE BITS RATE}} | {{FDBK TUNE DAMP MIX}}. Distortion with four curves, bit and sample-rate
reduction, and a tuned feedback path that turns it into a resonator.</li>
<li>{{TALK}}: {{VOWL SHFT RESO MIX}} | {{RATE DPTH ENV BITE}}. A vowel filter. The vowel can be moved by hand, by its
own LFO, or by how hard you play.</li>
<li>{{MODFX}}: chorus, flanger, phaser and rotary speaker, a rate and a depth each ({{CH.R CH.D}} | {{FL.R FL.D}} |
{{PH.R PH.D}} | {{RO.S RO.D}}).</li>
</ul>""") + '<div class="shots">' + S.svg("TRK FX", 3, "DIRT") + S.svg("TRK FX", 4, "TALK") + S.svg("TRK FX", 5, "MODFX") + "</div>"),
        ("Insert effects", fmt("""
<p>The {{FX}} page shows one insert slot: {{SLOT}} chooses which, {{TYPE}} the effect, {{WET}} how much of it, and
knobs 5 to 8 are the effect's own four controls.</p>
<div class="note"><b>Careful</b>Turning {{TYPE}} replaces the effect and its four values at once, without the four-click
confirmation. Undo ([[Func]] + [[Shift]] + [[Play]]) brings the effect back as it was; a scroll through the list
counts as one step.</div>""") + table(["TYPE", "Effect", "Knobs 5–8"], fx_rows, "cols3")),
        ("Tempo sync", fmt("""
<p>An insert that has a {{TIME}} (the delays) or a {{RATE}} (chorus, flanger, phaser, tremolo, auto-pan, ensemble,
ring modulator, vibrato, wah) shows {{SYNC}} in cell 3 in place of {{ON}}. With {{SYNC}} on, that cell picks one of
22 divisions from 1/128 to 16 bars, triplets and dotted included, and follows the tempo. The free value is kept for
when you switch sync off. A division too long for the effect is halved until it fits. To take a synced effect out,
turn {{WET}} to 0.</p>""")),
        ("Sends and master", fmt("""
<p>{{SND1}} and {{SND2}} on each track feed two buses shared by all tracks: a reverb and a delay to begin with.
{{BUS1}} and {{BUS2}} on the {{TRACK}} page change what each bus carries, from the same list of effect types.</p>
<p>The master EQ, compressor and limiter are on {{SETTINGS}} → {{MASTR}}; master volume is {{MVOL}} on {{SETUP}}.</p>
<h3>Sidechain ducking</h3>
<p>The {{DUCK}} insert turns its track down when another track plays: {{SRC}} chooses which ({{SELF}} makes it an
ordinary compressor), {{THRS}} and {{RATI}} how hard, {{RELS}} how fast it comes back.</p>""")),
    ]))

    # =========================================================================================
    ch.append(("The sequencer", [
        ("Patterns and tracks", fmt("""
<p>A project has 16 patterns. Each pattern holds the steps of every track; each track has its own length ({{LN}}, 1
to 1024 steps) and speed ({{SP}}: quarter, half, normal, double, quadruple). A track has one sound for all patterns.</p>
<p>Hold [[&gt;]] and press a pad to queue a pattern. While playing, the change happens at the end of the loop;
stopped, at once.</p>""")),
        ("Steps", fmt("""
<p>On {{STEP}}, a tap adds or removes a step and a held step can be edited. Hold a step and:</p>
<ul>
<li>press keys to set its note or chord;</li>
<li>keep holding: the display changes to the track's {{MAIN}} page showing that step's values, and any knob you
turn is locked on that step;</li>
<li>use the knob-page buttons to reach the other sound pages, and, one press back from {{MAIN}}, its {{TRIG}} page:
{{NOTE}}, {{VEL}}, {{LEN}} (length), {{MICR}} (micro-timing, up to half a step early or late), and the condition
cells.</li>
</ul>""")),
        ("Parameter locks", fmt("""
<p>A lock is a knob value that applies to one step only. Hold a step on {{STEP}}, {{PARAM}}, {{TRK FX}} or {{FLTR}}
and turn a knob. While the step is held the cells show that step's values and the name bar shows its number
({{S5}}). After the step, the knob returns to its normal value. Engine controls, mixer levels and sends,
effect blocks and inserts, and the arp's gate, velocity and ratchet can all be locked.</p>
<p>Remove one lock: hold the step, hold [[&gt;]], turn the knob. Remove all of a step's locks: {{LCKS}} on its
{{TRIG}} page, anticlockwise (an undo step).</p>
<p><b>Pressure into locks.</b> On a step with recorded pad pressure, {{>LCK}} on its {{PRES}} page (four clicks)
writes the swell as locks: on every step the note spans, each cell the track's pressure routes point at is locked
to where the average pressure over that step would hold it. A route's depth is how far full pressure moves the cell
from its own value toward its top (toward its bottom when the depth is negative). Steps under the note that had no
trig become trigs that carry only locks, with no note of their own. Chords on a synth track share the cells, so
their pressures are averaged; on a drum track each pad's own cell is locked. A route with no cell to lock (vibrato;
{{FM}} on the FM synth; {{TIMB}} on the macro oscillator) is left out and the name bar says so
(<code>2 OF 3 ROUTES</code>); if no route can be locked, nothing is changed (<code>NO ROUTE</code>). The recorded
pressure is then removed: from here on the swell is edited like any other lock.</p>""")),
        ("Trig conditions", table(["COND", "The step plays…"], [
            ("<code>ALL</code>", "every time"),
            ("<code>PROB</code>", fmt("with the chance set by {{PROB}}")),
            ("<code>A:B</code>", fmt("on pass {{A}} of every {{B}} passes of the track (1:2 = every other time, 4:4 = every fourth)")),
            ("<code>FILL</code> / <code>!FIL</code>", fmt("only while fill is held ([[Shift]] + [[Rec]]) / only while it is not")),
            ("<code>1ST</code> / <code>!1ST</code>", "only on the first pass after starting / on every pass but the first"),
            ("<code>PRE</code> / <code>!PRE</code>", "only if the previous conditional step played / only if it did not")])),
        ("Recording", fmt("""
<p><b>Start.</b> Hold [[Rec]] and press [[Play]]: recording goes on and the sequencer starts from the top. If it
is already playing it keeps playing and recording simply switches on. [[Rec]] alone switches recording on or off
without touching the transport (it acts when you let the button go), so [[Rec]], then [[Play]] works too.</p>
<p><b>While recording</b> the name bar shows <code>REC FREE</code> or <code>REC QUANT</code> on every screen (a
single <code>R</code> or <code>Q</code> when room is short) and a red light runs across the pads, one pad per step
of the selected track. Pads that play notes keep playing them under the light; on {{STEP}} the playhead is the
light, and recorded notes appear on the pads as you play them. Keys are written into the selected track, pad hits
(each repeat of a roll included) into the track the pads play, arp output into the arp's track. Notes played
together land on one step. A later pass adds to what is there; playing on a step that already holds notes replaces
them.</p>
<p><b>Pressure is recorded with pad notes.</b> What you do with a pad after striking it (press harder, ease off) is
recorded with that note, for as long as you hold the pad, and the note is given the length you held it. On playback
each note gets its own pressure back, through the track's pressure routes ({{AT ROUTE}}) as they are set at that
moment, or as polyphonic pressure on a MIDI track: two pads held together keep their own swells. The pressure
follows the tempo, the track's speed, micro-timing and swing with its note, and is let go when the note ends, when
the track is muted and when the sequencer stops. A pad played by hand over a recorded note of the same pitch has
the say while it is held. A note you do not press into stores nothing. A pad that is rolling ({{ROLL}}) uses its
pressure for the rate of the roll, so that pressure is not recorded; with a roll threshold set, a pad held below
the threshold is an ordinary note and its pressure is recorded. Keys have no pressure. A note can be at most one
pass of its track long; a pad held longer is recorded up to there. Recording over a step replaces its notes and
their pressure. See {{PRES}} on a held step's pages to scale, convert or remove what was recorded.</p>
<p><b>Free or quantized.</b> Press [[Rec]] + [[Play]] again while recording to change between the two; the choice
is kept for the next recording. <code>REC FREE</code> writes each note on the nearest step and keeps its distance
from the grid as micro-timing, so it plays back as you played it; {{QN}} on {{STEP}} pulls playback toward the
grid by any amount and can be turned back down later. <code>REC QUANT</code> writes each note exactly on the
nearest step.</p>
<p><b>Stop.</b> [[Play]] stops the sequencer and ends recording; [[Play]] again plays back what you recorded.
[[Rec]] alone ends recording and leaves the sequencer playing. Either way, if you recorded anything, {{STEP}} opens
with the track you recorded last selected, on the step page that holds its first recorded step: hold a step and
the display shows that track's {{MAIN}} page for it; turn a knob to lock a parameter on that step. (If pads are pinned, the pads stay the pinned ones until you unpin them.)</p>""")),
        ("Copy, keep, fill, tempo", fmt("""
<ul>
<li><b>Copy / clear / paste</b>: [[Func]] + [[Rec]] / [[Play]] / [[&gt;]], acting on the held step, the track
({{STEP}}), the pattern ({{TRACKS}}, clear and paste held for half a second), the sound ({{PARAM}}, {{FLTR}}) or
the effects ({{TRK FX}}).</li>
<li><b>Undo / redo</b>: [[Func]] + [[Shift]] + [[Play]] / [[Rec]], fifteen steps back. A recording is one step.</li>
<li><b>Keep and reload</b>: [[&gt;]] + [[Rec]] keeps a temporary copy of the pattern; [[&gt;]] + [[Play]] brings it
back, at once (it is meant to be played in time) and as an undo step. The copy lives until you quit.</li>
<li><b>Fill</b>: hold [[Shift]] + [[Rec]]; steps with the {{FILL}} condition play while you hold.</li>
<li><b>Tempo</b>: {{BPM}} on {{SETTINGS}}, knob 8 on {{STEP}}, or tap [[Func]] + [[Settings]].</li>
<li><b>Swing</b>: {{SWNG}} on {{SETTINGS}} → {{SETUP}}.</li>
<li><b>Mute and solo</b>: bottom pads on {{TRACKS}}, or hold [[▲]] / [[▼]] and press a bottom pad from anywhere.
A muted track keeps its place and comes back in time.</li>
</ul>""")),
        ("Song mode", fmt("""
<p>{{SONG}} is an ordered list of rows, each a pattern, a repeat count and a transposition for pitched tracks. Switch
{{SG}} on and [[Play]] follows the list; {{LP}} loops it.</p>""")),
        ("Clock", fmt("""
<p>{{CLK}} on {{SETTINGS}} sets MIDI clock: {{INT}} (Neptune is the clock), {{EXT}} (follow incoming clock) or
{{AUTO}}. {{LINK}} is listed for the future and does nothing yet.</p>""")),
    ]))

    # =========================================================================================
    arp_modes = ["Up", "Down", "Up-down", "Random", "As played", "Chord repeat", "Walk", "Play order", "Converge",
                 "Diverge", "Poly arp (all notes, accented)", "Random with octave jumps"]
    ch.append(("Arpeggiator and rolls", [
        ("The arpeggiator", fmt("""
<p>Every track has its own. [[Arp]] opens the screen; pressed again there, it switches the selected track's arp on.
[[Shift]] + [[Arp]] latches it so it carries on after you let go (with the arp off, the same gesture latches the
keys themselves). Arps follow the transport, the scale lock and the tempo.</p>""")
            + table(["Page", "Knobs 1–4", "Knobs 5–8", "For"], [
                ("<code>ARP</code>", "MODE RATE OCT GATE", "RPT SWNG GLID LTCH", "The basics"),
                ("<code>PLAY</code>", "STRM TIME DIR VEL", "RTCH RDEC HUMN FREZ", "Strumming, ratchets, humanize, freeze"),
                ("<code>VARY</code>", "SPCE DICE DENS SCAT", "RCHN GVAR STRP VAR", "Variation macros"),
                ("<code>MUTA</code>", "MUTA INTV MODE SPCE", "EVOL ANC &gt;PAT", "Mutation over time"),
                ("<code>ACFG</code>", "DEST DPTH REC LOCK", "TAP RHYT", "Pressure, recording, tapped rhythm"),
                ("<code>ROLL</code>", "ROLL MODE BASE MAX", "DPTH GATE P&gt;V DCAY", "Pad rolls"),
                ("<code>ROL2</code>", "SWNG GLID THRS LTCH", "HUMN ACNT PROB RPTS", "Pad rolls, continued")], "cols3")
            + "<p>Modes: " + "; ".join(arp_modes) + ".</p>"
            + fmt("""
<h3>Variation</h3>
<p>The {{VARY}} macros change what the arp plays without changing what you hold: {{DENS}} thins it out, {{SCAT}}
throws notes up and down octaves, {{RCHN}} adds ratchets, {{GVAR}} varies note lengths, {{SPCE}} sets how bold all
of that is and {{DICE}} re-rolls it. {{STRP}} hands one of them to the mod strip.</p>
<h3>Mutation</h3>
<p>With {{MUTA}} on, the pattern changes itself every {{INTV}} bars. {{MODE}} decides whether each change starts
from the ancestor again ({{ANC}}) or builds on the last ({{CUM}}). {{EVOL}} shows the generation: turn it up to
mutate now, down to go back to the ancestor. {{>PAT}} (four clicks) writes what the arp is playing into the
track's pattern as ordinary steps. Going back to the ancestor and {{>PAT}} can both be undone.</p>
<h3>Tapped rhythm</h3>
<p>Switch {{TAP}} on and tap a rhythm on any pad (they light amber). Switch it off, switch {{RHYT}} on, and the arp
plays its notes in your rhythm.</p>
<h3>Locking the arp</h3>
<p>{{TRK FX}} → {{ARPLK}} holds the arp's gate, velocity and ratchet as lockable cells, so steps of the pattern can
change how the arp behaves as it passes.</p>""")),
        ("Pad rolls", fmt("""
<p>With {{ROLL}} on, any pad that plays a note ({{DRUMS}}, {{SCALE}}, {{AT ROUTE}}, {{PRESET}}) repeats while held.
Pressure raises the rate from {{BASE}} to {{MAX}}: tempo divisions in {{QUANT}} mode, free Hz in {{FREE}} mode.
{{P>V}} lets pressure set the loudness of each repeat; {{DCAY}} fades them.</p>
<p>{{ROL2}} refines it: swing, a glide between rates, a pressure threshold so light touches play single hits
({{THRS}}), latch, humanize, an accent every Nth hit, a probability per repeat and a limit on the number of repeats.
Rolls are recorded hit by hit.</p>""")),
    ]))

    # =========================================================================================
    ch.append(("Sampling, slicing and stretch", [
        ("Samples", fmt("""
<p>The sampler's {{SMPL}} cell chooses from the bank: a built-in drum loop and eight hits, any WAV or AIFF file in
<i>Music / Neptune / Samples</i> (read at start-up), and your resampled takes. Set start, end and loop on the
{{SAMPL}} page or by eye on {{WAVE}}; [[Func]] + turn is coarse and [[Shift]] + turn fine, and zooming ({{ZM}})
makes the markers finer still.</p>
<p>Play modes: {{FWD}}, {{REV}}, {{1SHT}} (plays to the end regardless of the key), {{GATE}} (stops on release),
{{LOOP}}, {{PING}} (loops back and forth), {{GRAN}}.</p>
<p>{{ROOT}} is the key at which the sample plays at its recorded pitch; {{NORM}} brings a quiet sample up to full
level.</p>""") + '<div class="shots">' + S.svg("WAVE", 1, "WAVE") + S.svg("PARAM sampler", 2, "Sampler, SAMPL") + "</div>"),
        ("Slicing", fmt("""
<p>The {{SLICE}} page cuts the sample into pieces you can play separately.</p>
<ul>
<li>{{MODE}} = {{GRID}}: {{SLCS}} equal slices. Right for a loop that is already in time.</li>
<li>{{MODE}} = {{TRNS}}: one slice per hit, found by listening for the sudden rise at the start of each sound.
{{SENS}} sets how quiet a hit may be and still count; {{GAP}} sets the shortest time between two hits. {{SLCS}}
shows how many were found, and turning it down keeps only the strongest. Every slice starts on a zero crossing, so
slices do not click. Hits closer together than about a fiftieth of a second are heard as one.</li>
</ul>
<p>{{PLAY}} decides which slice a note plays:</p>
<ul>
<li>{{ONE}}: the slice chosen with {{SLIC}} (or pad N on {{WAVE}}). Lock {{SLIC}} per step to re-order a loop.</li>
<li>{{KEYS}}: each key plays its own slice, going up from {{ROOT}}. The loop is laid out across the keyboard.</li>
<li>{{NEXT}}: every new note plays the next slice, so a row of identical steps walks through the loop.</li>
<li>{{RAND}}: a random slice per note.</li>
</ul>
<p>{{FADE}} softens the end of each slice and {{SLEN}} shortens them all for a gated feel. The {{WAVE}} screen draws
a tick at each slice and lights the pad of the slice that is sounding.</p>
<div class="note"><b>Not lockable</b>{{SMPL}}, {{NORM}} and the cells that re-cut the sample ({{MODE}}, {{SLCS}},
{{SENS}}, {{GAP}}) cannot be locked to a step or driven by pressure: each change re-analyses the whole sample.
{{SLIC}}, {{PLAY}}, {{FADE}} and {{SLEN}} can.</div>""") + '<div class="shots">' + S.svg("PARAM sampler", 4, "Sampler, SLICE") + "</div>"),
        ("Time stretch", fmt("""
<p>The {{STRCH}} page changes how long the sample lasts without (except in {{TAPE}}) changing its pitch. The key
still sets the pitch; start, end and loop points, play modes, filter, envelope and LFOs all work as usual. Each
{{ALGO}} has a sound of its own, and that is the point of having several.</p>""")
            + table(["ALGO", "How it works", "Sounds like"], [
                ("<code>TAPE</code>", "Plays faster or slower, like tape or vinyl.", "Pitch drops as it slows. With SYNC, the classic repitched loop."),
                ("<code>CYCL</code>", "Repeats short cycles of the sound, crossfading from one to the next.", "The metallic, buzzing stretch of early hardware samplers. SIZE tunes the buzz."),
                ("<code>GRAN</code>", "Overlapping grains from the granular engine.", "Smooth and slightly diffuse. Good on pads, vocals, textures."),
                ("<code>SLCE</code>", "Does not stretch anything: each slice plays untouched and is started when its turn comes.", "Tight and punchy on drums. Slowing down leaves gaps between hits; speeding up cuts them short."),
                ("<code>SMER</code>", "Very long grains from random nearby positions, at a sixteenth of the shown speed.", "A frozen wash. A second of sound becomes a slowly moving chord."),
                ("<code>WSOL</code>", "Like CYCL, but each cycle is lined up with the last by matching their waveforms.", "The cleanest of the set on single notes and voices.")], "cols3")
            + fmt("""
<p>{{SPD}} sets the speed from a quarter to four times. With {{SYNC}} on, the sample between its start and end
markers is made to last exactly {{LEN}} (a sixteenth of a bar to 32 bars) at the current tempo, and follows the
tempo if you change it: a loop recorded at any speed locks to the sequencer. {{SIZE}}, {{XFAD}} and {{JIT}} shape the
grain of the stretch, and {{SPRD}} widens {{GRAN}} and {{SMER}}.</p>
<p>The LFO destination {{SPD}} moves the speed by up to two octaves either way: tape-stop dives, or a stretch
that breathes.</p>
<p>In {{GRAN}} play mode the grain page is in charge and {{ALGO}} is ignored, but {{SYNC}} still works: {{SCAN}} at
1.00 then means the region lasts {{LEN}}.</p>""") + '<div class="shots">' + S.svg("PARAM sampler", 5, "Sampler, STRCH") + "</div>"),
        ("Granular", fmt("""
<p>In {{GRAN}} mode the sampler plays the sample as a cloud of short grains. Pitch comes from the key; time is
separate. The {{GRAIN}} page:</p>
<ul>
<li>{{SIZE}} and {{DENS}}: how long each grain is and how many start per second. Long and dense is smooth; short and
sparse is granular in the gritty sense.</li>
<li>{{SCAN}}: how fast the read point travels through the sample. 1× is normal speed, 0 freezes on the spot, negative
runs backwards. With {{SCAN}} at 0, {{STRT}} is the position: turn it, lock it, or put an LFO on {{GPOS}}.</li>
<li>{{SPRY}}: scatters grain positions and timing. {{PJIT}}: random pitch per grain.</li>
<li>{{SPRD}} stereo spread, {{SHAP}} the grain's envelope, {{RVRS}} the share of grains played backwards.</li>
</ul>
<p>The {{WAVE}} screen shows each grain as a tick while it sounds.</p>""") + '<div class="shots">' + S.svg("PARAM sampler", 3, "Sampler, GRAIN") + "</div>"),
        ("Resampling", fmt("""
<p>The {{REC}} page of {{WAVE}} records Neptune's own sound into a new sample.</p>
<ol>
<li>{{SR}}: the source, the master output or one track after its effects.</li>
<li>{{LN}}: free length, or a set length from one beat to eight bars. {{QN}}: start now or on the next bar.
{{TH}}: optionally wait for the source to pass a level.</li>
<li>{{RC}}: two clicks clockwise within a second arms and starts; one click anticlockwise stops. The top pads are
red while recording.</li>
<li>The take is saved as <i>R001.wav</i>, <i>R002.wav</i>… in <i>Music / Neptune / Samples</i> and given to the
selected track: as its sample on a sampler track, as its {{USER}} table on a wavetable track.</li>
</ol>
<p>{{NM}} normalises takes, {{SM}} picks a different sample for the track, and {{CR}} (four clicks) crops the sample
to its markers.</p>""") + '<div class="shots">' + S.svg("WAVE", 2, "WAVE, REC page") + "</div>"),
    ]))

    # =========================================================================================
    ch.append(("Projects, presets and the rest", [
        ("Projects", fmt("""
<p>A project is everything: tempo, all 16 patterns with their steps and locks, every track's engine and sound,
effects, sends, master section, song, scale pages, pressure routes and ranges, and arp settings. There are 32 slots
(<i>Music / Neptune / Projects</i>, P01 to P32).</p>
<p>{{SETTINGS}} → {{PROJ}}: {{SLOT}} chooses, {{LOAD}}, {{SAVE}}, {{SVAS}} (save as, to the chosen slot) and {{NEW}}
each need four clicks and answer OK or FAIL. {{AUTO}} keeps saving to the current slot as you work. The app reopens
the last project when it starts.</p>
<p>Not saved: an arp's mutation state, the temporary pattern copy, the clipboards, and the keyboard split.</p>""")),
        ("Presets", fmt("""
<p>A preset is one track's sound. Thirty are built in; yours are saved from {{PRESET}} with {{SV}} into
<i>Music / Neptune / Presets</i>. Categories: BASS, PAD, LEAD, KEYS, FX, PERC, KIT, INIT.</p>""")),
        ("Keyboard split", fmt("""
<p>[[Shift]] + [[Oct−]] drops the lowest twelve keys by one further octave, then two, then three, then back to
normal; [[Shift]] + [[Oct+]] steps the other way. The name bar shows {{SPLIT -1 OCT}}. Plain [[Oct−]] / [[Oct+]]
still shift the whole keyboard.</p>""")),
        ("Latency and headroom", fmt("""
<p>Neptune adds about 1.3 ms of its own between a key and its sound. What you hear beyond that is the audio output:
an interface or wired headphones add a few milliseconds, the Mac's built-in speakers about 19 ms.</p>
<p>The heaviest test session (eight tracks, eight notes each, every effect block plus delay and reverb on every
track) uses about a quarter of one processor core on an M3 Pro. You are unlikely to run out.</p>
<p>The display redraws about twelve times a second, which is as fast as the Launchkey accepts pictures.</p>""")),
        ("If something seems wrong", table(["What you see", "What to check"], [
            ("No sound", fmt("{{MVOL}} on {{SETTINGS}}; the track's {{VOL}}; whether it is muted (red pad on {{TRACKS}}) or another track is soloed (blue).")),
            ("Pads do not play the drums", fmt("Only {{DRUMS}} has drum pads: [[Shift]] + bottom pad 2. The keys play the selected drum track on every screen.")),
            ("A knob does not seem to change the sound", fmt("A step lock or a pressure range may be overriding it. Orange pads on {{PARAM}} have locks; a violet pad has a range.")),
            ("Pads play notes on a screen where they should not", fmt("They are pinned ({{PIN}} in the name bar). On {{DRUMS}} or {{SCALE}}, hold [[Shift]] and hold that screen's bottom pad for half a second.")),
            ("Notes hang or the arp will not stop", fmt("Latch is on ({{LATCH}} in the name bar): [[Shift]] + [[Arp]].")),
            ("Low keys are in the wrong octave", fmt("A keyboard split is on: [[Shift]] + [[Oct+]] until the {{SPLIT}} message says off.")),
            ("Copy or paste did nothing", fmt("They act only on {{STEP}}, {{TRACKS}}, {{PARAM}}, {{FLTR}} and {{TRK FX}}, or on a held step. On {{TRACKS}} clear and paste need the button held for half a second.")),
            ("A synced delay is shorter than the division says", "The division was longer than the delay can hold and has been halved to fit."),
            ("The display is blank after plugging in", "Quit and restart the app with the Launchkey already connected.")])),
    ]))

    # =========================================================================================
    ch.append(("Short names", [
        (None, fmt("""
<p>Every short name the display can show on a knob, in alphabetical order. <b>Belongs to</b> names the engine,
effect or screen and, after the colon, the page. Names that mean different things in different places have one row
per meaning. Values shown as 0–100 are the display's scale.</p>""") + ctx["glossary"])]))

    # =========================================================================================
    cheat = table(["To…", "Do this"], [
        ("Go to a screen", fmt("[[Shift]] + pad")),
        ("Next / previous knob page", "Knob-page buttons"),
        ("Jump to a page", fmt("[[Func]] + top-row pad")),
        ("Select a track", fmt("Hold [[▲]] or [[▼]] + top pad N; or [[Shift]] + [[▲]] / [[▼]]; or top pad on {{TRACKS}}")),
        ("Mute / solo a track", fmt("Hold [[▲]] or [[▼]] + bottom pad N / [[Func]] + bottom pad on {{TRACKS}}")),
        ("Move a control on all tracks", fmt("Hold [[▲]] or [[▼]] + turn")),
        ("See what Shift or Func does here", fmt("Hold [[Shift]] or [[Func]] alone; knob-page buttons turn the cards")),
        ("Fine / coarse", fmt("[[Shift]] + turn / [[Func]] + turn")),
        ("Add or remove a step", fmt("Tap a pad on {{STEP}}")),
        ("Set a step's note", "Hold the step + key"),
        ("Lock a knob to a step", "Hold the step + turn"),
        ("Remove that lock", fmt("Hold the step + [[&gt;]] + turn")),
        ("Change pattern", fmt("Hold [[&gt;]] + pad")),
        ("Keep / reload the pattern", fmt("[[&gt;]] + [[Rec]] / [[&gt;]] + [[Play]]")),
        ("Copy / clear / paste", fmt("[[Func]] + [[Rec]] / [[Play]] / [[&gt;]] (a whole pattern: hold half a second)")),
        ("Undo / redo", fmt("[[Func]] + [[Shift]] + [[Play]] / [[Rec]]")),
        ("Record", fmt("Hold [[Rec]] + [[Play]]; again = free / quantized; [[Play]] stops and shows the steps")),
        ("Fill", fmt("Hold [[Shift]] + [[Rec]]")),
        ("Stop and rewind", fmt("[[Shift]] + [[Play]]")),
        ("Tap tempo", fmt("[[Func]] + [[Settings]], tapped")),
        ("Arp on / off", fmt("[[Arp]] (opens the screen), [[Arp]] again")),
        ("Latch", fmt("[[Shift]] + [[Arp]]")),
        ("Scale lock / scale screen", fmt("[[Scale]] / [[Shift]] + [[Scale]]")),
        ("Pressure range on a knob", fmt("Turn it, then [[Func]] + [[▲]] (high end) or [[▼]] (low end); again clears")),
        ("Pin the playing pads", fmt("On {{DRUMS}} or {{SCALE}}: [[Shift]] + hold that screen's bottom pad half a second")),
        ("Confirm something destructive", "Four clicks clockwise on its knob"),
        ("Split the keyboard", fmt("[[Shift]] + [[Oct−]] / [[Oct+]]"))], "keys")
    ch.append(("Gestures at a glance", [(None, cheat)]))

    # =========================================================================================
    divs = choices(params, "FM Synth", "LFO1", "DIV")
    fxdiv = ["1/128", "1/64", "1/32T", "1/32", "1/16T", "1/32D", "1/16", "1/8T", "1/16D", "1/8", "1/4T", "1/8D", "1/4",
             "1/2T", "1/4D", "1/2", "1/2D", "1BAR", "2BAR", "4BAR", "8BAR", "16BAR"]
    lists = table(["List", "Values, in order"], [
        ("Engines (<code>ENG</code>)", "Synth · FM · Macro oscillator · Wavetable · Wave sequencer · Sampler · Drums · MIDI track"),
        ("Oscillator waves", " ".join(choices(params, "Unified Voice", "OSC", "WAV1"))),
        ("Filter types", " ".join(choices(params, "Unified Voice", "FILTR", "TYPE"))),
        ("LFO shapes (<code>SHPE</code>)", " ".join(lfo_shapes)),
        ("LFO trigger modes (<code>TRIG</code>)", " ".join(choices(params, "FM Synth", "LFO1", "TRIG"))),
        ("LFO divisions (<code>DIV</code>)", " ".join(divs) + " (D = dotted, T = triplet)"),
        ("Effect sync divisions", " ".join(fxdiv)),
        ("Insert effect types", " ".join(f["name"] for f in params["effects"])),
        ("Drum types", " ".join(choices(params, "Drum", "DR 1", "TYPE"))),
        ("Sampler play modes", " ".join(choices(params, "Sampler", "SAMPL", "MODE"))),
        ("Slice play modes", " ".join(choices(params, "Sampler", "SLICE", "PLAY"))),
        ("Stretch algorithms", " ".join(choices(params, "Sampler", "STRCH", "ALGO"))),
        ("Stretch lengths (bars)", " ".join(choices(params, "Sampler", "STRCH", "LEN"))),
        ("Wavetable scan modes", " ".join(choices(params, "Wavetable", "SCAN", "MODE"))),
        ("Wavetable frame options", " ".join(choices(params, "Wavetable", "SCAN", "FRMS"))),
        ("Grain shapes", " ".join(choices(params, "Sampler", "GRAIN", "SHAP"))),
        ("Wavetables", " ".join(choices(params, "Wavetable", "WAVE", "TABL"))),
        ("Macro models", " ".join(m for m, _, _ in ctx["macro_models"])),
        ("FM algorithms", " ".join(html.escape(c) for c in choices(params, "FM Synth", "FMMIX", "ALGO"))),
        ("Trig conditions", "ALL PROB A:B FILL !FIL 1ST !1ST PRE !PRE"),
        ("Arp modes", "; ".join(arp_modes)),
        ("Arp rates", "1/1 1/2 1/4 1/4T 1/8 1/8T 1/16 1/16T 1/32 1/32T 1/64 1/64T (the knob runs slowest to fastest)"),
        ("Roll divisions", "1/2 1/4 1/4T 1/8 1/8T 1/16 1/16T 1/32 1/32T 1/64 1/64T"),
        ("Track speeds", "1/4 1/2 1x 2x 4x"),
        ("Scale pages (built in)", "Chrom Major Minor Penta Blues Whole Dorian Mixo HrMin mPent Phryg Lydia"),
        ("Preset categories", "ALL INIT BASS PAD LEAD KEYS FX PERC KIT"),
        ("Clock sources", "INT EXT AUTO LINK")], "keys")
    ch.append(("Value lists", [(None, lists)]))

    # =========================================================================================
    terms = table(["Term", "Meaning"], [
        ("Bar screen", "A screen with a picture or list and a strip of eight two-letter knob labels along the bottom."),
        ("Bus", "One of two shared effects that every track can send to."),
        ("Cell", "One of the eight positions on the display; the knob in the same position edits it."),
        ("Condition", "A rule on a step that decides whether it plays on a given pass."),
        ("Engine", "The sound generator of a track."),
        ("Fill", "A held state in which steps marked FILL play."),
        ("Four clicks", "The confirmation for anything destructive: four clockwise clicks on its knob."),
        ("Grain", "A short slice of a sample, played many at a time in granular mode."),
        ("Grid screen", "A screen with eight cells in two rows of four."),
        ("Insert", "An effect in a slot on one track."),
        ("Knob page", "One set of eight cells; the knob-page buttons move between them, going round at the ends."),
        ("Latch", "Notes or an arp carrying on after the keys are released."),
        ("Lock (parameter lock)", "A knob value that applies to one step only."),
        ("Micro-timing", "A step's offset from the grid, up to half a step either way."),
        ("Name bar", "The top line of the display."),
        ("Page key", "Func + a top-row pad: jumps straight to a page."),
        ("Pattern", "One of 16 sets of steps for all tracks."),
        ("Pin", "Keeping the DRUMS or SCALE pads playable on every screen."),
        ("Pressure", "Polyphonic aftertouch: how hard each pad is pressed after it is struck, per pad."),
        ("Pressure range", "A range on a knob that a bottom-row pad sweeps with pressure."),
        ("Ratchet", "Several quick repeats inside one step."),
        ("Resampling", "Recording Neptune's own output into a new sample."),
        ("Roll", "A pad repeating while held, faster with pressure."),
        ("Route", "A pressure destination and depth on a track."),
        ("Row", "One line of the song: a pattern, a repeat count and a transposition."),
        ("Send", "The amount of a track fed to a bus."),
        ("Slice", "One piece of a sample, cut evenly or at a hit."),
        ("Time stretch", "Changing how long a sound lasts without changing its pitch."),
        ("Transient", "The sudden start of a sound, such as a drum hit; where transient slicing cuts."),
        ("Step", "One position in a track's pattern."),
        ("Track layer", "What is available while ▲ or ▼ is held: track select, mute, and all-track turns."),
        ("Trig", "A step that is switched on.")], "keys")
    ch.append(("Glossary of terms", [(None, terms)]))
    return ch


def ctx_diagram(ctx):
    return f'<div class="diagram">{ctx["controller_svg"]}</div>'
