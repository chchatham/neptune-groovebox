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
    text = re.sub(r"\{\{(.+?)\}\}", lambda m: f"<code>{html.escape(m.group(1))}</code>", text)
    # (A name split over two lines is not matched and would be printed with its braces.)
    left = re.search(r"\{\{|\}\}|\[\[|\]\]", text)
    if left:
        raise SystemExit("chapters.py: a [[key]] or {{name}} is split over a line: ..." + text[max(0, left.start() - 60):left.start() + 60])
    return text


def step(n, title, body):
    return f'<div class="step"><div class="k">{n}</div><div class="b"><h3>{title}</h3>{body}</div></div>'


def table(head, rows, cls=""):
    h = "".join(f"<th>{c}</th>" for c in head)
    b = "".join("<tr>" + "".join(f"<td>{c}</td>" for c in r) + "</tr>" for r in rows)
    return f'<table class="{cls}"><thead><tr>{h}</tr></thead><tbody>{b}</tbody></table>'


def fx_choices(params, effect, label):
    for f in params["effects"]:
        if f["name"] == effect:
            for s in f["specs"]:
                if s["label"] == label:
                    return s["choices"]
    raise SystemExit(f"no choices for the insert {effect}'s {label}")


def choices(params, engine, page, label):
    for e in params["engines"]:
        if e["name"] == engine:
            for p in e["pages"]:
                if p["name"] == page:
                    for s in p["specs"]:
                        if s["label"] == label:
                            return s["choices"]
    raise SystemExit(f"no choices for {engine}/{page}/{label}")


# The SRC / VIA lists of the engines on the modulation router: the router's own places (the list they all begin
# with) and each engine's own after them, read from the dump so that the text keeps their order.
ROUTED = (("FM", "FM Synth"), ("Macro", "Macro Oscillator"), ("Wavetable", "Wavetable"), ("Wave seq", "Wave Sequencer"),
          ("Sampler", "Sampler"))
# (The synth's list has LFO3 LFO4 and ENV2 .. ENV5 in it, after ONE; the drum kit and the resonator have the
# router's list and nothing of their own.)
PRESET_ENGINES = ("Synth", "FM", "Macro", "Wavetable", "Wave seq", "Sampler", "Drums", "MIDI", "Resonator")


def preset_table(params):
    """Every built-in preset by category and engine, in the order PRESET lists them (read from PresetManager)."""
    by = {}
    for p in params["presets"]:
        by.setdefault(p["category"], {}).setdefault(p["engine"], []).append(p["name"])
    rows = []
    for category, engines in by.items():
        for engine in sorted(engines):
            names = engines[engine]
            rows.append((f"<code>{html.escape(category)}</code>", PRESET_ENGINES[engine], str(len(names)),
                         ", ".join(html.escape(n) for n in names)))
    return table(["Category", "Engine", "", "Presets"], rows, "cols3")


def preset_count(params, category=None):
    return sum(1 for p in params["presets"] if category is None or p["category"] == category)


# What each built-in multi shows, what to play and which control to move to hear its point (the names and the
# members are read from the dump; a multi without a line here stops the build).
MULTI_TEXT = {
    "Duck Pair": "A kit playing four on the floor and a pad that ducks under it: the pad's {{DUCK}} insert has its {{SRC}} on the kit's track. Press [[Play]] and hold a chord (the keys play the pad). On the pad's {{TRK FX}} → {{FX}} page lower {{THR}} or raise {{RATI}}: it pumps deeper.",
    "DrumHarp": "The kit strikes four strings: a resonator of strings takes the kit as its exciter, through the track input. Press [[Play]]: the pattern holds a chord of four strings and every hit of the kit rings it; stop the kit and the strings are silent. Play your own chords on the keys (the resonator). {{IN}} on the resonator's {{EXC}} page is how hard the kit strikes them.",
    "VoxPlate": "A drum break through a sheet of metal: a resonator plate with the break as its exciter. Press [[Play]] for the break and play keys for the plate's pitch; alone the plate makes no sound. Turn {{MRPH}} on the resonator's {{RSN}} page: another body under the same break.",
    "RobtBrk": "The break speaks a chord: four detuned saws a note through a {{VOC}} insert, the break its modulator. Press [[Play]] and hold a chord: the chord is heard only where the break sounds, and is silent without it. The mod strip muffles the voice.",
    "RingVox": "A pad multiplied by a wavetable: the pad's input is in {{MODE}} {{RING}}, and the vowel table on the other track is the other side of the ring modulator. Press [[Play]] and hold a chord: what you hear are the sums and differences of the two sounds' partials. Turn {{POS}} on the wavetable track's {{WAVE}} page and every sideband moves; the strip opens the pad's filter.",
    "SmplFM": "A drum break as a seventh operator: the break, at audio rate, is on the phase of every operator of an FM bell (its {{XIN}} page, both routes on {{PMAL}}, and two slots more on the modulator). Press [[Play]] and play the bell: each hit of the break tears it into grain, and between hits it is clean. Turn {{AMT1}} and {{AMT2}} on the bell's {{XIN}} page to 0 and back.",
    "PumpPad": "The kick's loudness moves the pad with no compressor anywhere: two mod slots from the source {{IN}} (the level of the pad's input), one taking its level down, one closing its filter. Press [[Play]] and hold a chord. On the pad's {{SLT1}} page the two {{AMT}} cells are how far it ducks and how far it darkens.",
    "Key CMP": "An ordinary compressor on the pad, keyed from the kit's track: the {{CMP}} insert's {{KEY}} is that track. Press [[Play]] and hold a chord: the kick takes the pad down by about 12 dB. {{THR}} and {{RATI}} on the insert's page set how far.",
    "VocTune": "The break speaks through a tuned body: a {{VOC}} insert in {{MODE}} {{TUNE}}, where the break's sixteen band levels are the gains of a bank of modes tuned to the notes you play. Press [[Play]], play one note, then another: the body's pitch follows the keys. The strip muffles it.",
    "Mod Dly": "A track's own modulation moving the cells of its own insert effects (the destinations {{1:C1}} …). On the FM pluck, {{LFO1}} leans on its tape echo's {{TIME}}, so every repeat comes back a little sharp or flat, and a slot sends pad pressure to the echo's {{WET}}; the second track is a pad through a filter effect whose cutoff its {{LFO2}} sweeps once in two bars. Play the pluck from pads ({{SCALE}}): softly it is nearly dry, pressed it is thrown into the echo. {{DPTH}} on its {{LFO1}} page is the wobble.",
    "Rvb Pump": "A hall reverb on the pad and, after it in the same insert chain, a compressor keyed from the kit: the tail itself pumps. Press [[Play]], play a chord and let go: the reverb ducks on every kick. {{THR}} of the {{CMP}} in slot 2 sets how far.",
    "Sync Saw": "A lead whose saw is restarted by the bass: the sub on the other track, at audio rate, is on {{SYNC}} of the lead (its {{XIN}} page, {{DST1}}). Press [[Play]] for the bass and play the lead over it: whatever note you play, its overtones are the bass note's, and they move when the bass does. Turn {{AMT1}} on the lead's {{XIN}} page to 0: an ordinary saw again.",
    "HowlLoop": "Two tracks that listen to each other: each adds the other to what it plays (the input in {{MODE}} {{MIX}}), with a wavefolder and a short tape echo on the way round. Play one short note: the pair howls on by itself. {{LEVL}} on the two {{IN}} pages is the gain of the loop: turned down, a note rings round a few times and dies. The mod strip drives the fold and bends the howl.",
    "BandDuck": "Another track's spectrum as control signals: the sources {{BND1}} to {{BN16}} are the sixteen band levels of the track input. The pad ducks under the kit's three lowest bands alone, and the bands a clap fills open its filter. Press [[Play]] and hold a chord: the kick takes the pad down, the clap is a flash of brightness in it. On the pad's {{SLT1}} page give a slot another band as its {{SRC}} and hear which drum moves it.",
    "HitSweep": "Another track's hits as triggers: the pad's {{LFO1}} is a one-shot that waits for a sound to begin in the track input ({{TRIG}} {{XONE}}). Press [[Play]] and hold a chord: every hit of the kit throws the filter open and it falls shut in half a second; between hits nothing moves. Set {{TRIG}} on the pad's {{LFO1}} page to {{ONE}} and only your own keys start it.",
}


def multi_table(params):
    rows = []
    for m in params["multis"]:
        if m["name"] not in MULTI_TEXT:
            raise SystemExit(f"chapters.py says nothing about the multi preset {m['name']}")
        members = " + ".join(("<b>%s</b>" if i + 1 == m.get("play") else "%s") % PRESET_ENGINES[x["engine"]]
                             for i, x in enumerate(m["members"]))
        rows.append((html.escape(m["name"]), members, fmt(MULTI_TEXT[m["name"]])))
    gone = sorted(set(MULTI_TEXT) - {m["name"] for m in params["multis"]})
    if gone:
        raise SystemExit("chapters.py describes multi presets that are not built in: " + ", ".join(gone))
    return table(["Multi", "Tracks (the keys play the bold one)", "What it shows, what to play, what to move"], rows, "cols3")


# The built-in presets that each show one thing the instrument does (FusionPresets.h): what it is, what to play,
# what to move. A name here that the dump does not have stops the build.
SHOW_TEXT = {
    "BowCello": "A bow on a string. Play it from pads ({{SCALE}}) and lean in: pressure is the bow (route 1 on {{EXC}}) and opens the string's top (route 2 on {{DAMP}}); both are on {{AT ROUTE}}. The strip is vibrato.",
    "BowGlass": "A bow on a bell, a singing bowl. Pressure is the bow, and it moves the body itself along {{MRPH}}: pressed down, a drum head's partials come in under the bell.",
    "BrthPipe": "Breath into a tube. Pressure is the breath, and pressed hard it takes the exciter two points along {{XMPH}}, from breath through a scrape to a brush: the pipe is overblown and turns to noise.",
    "ScrapBar": "A scrape along a bar, dry guiro to shrieking metal. Pressure is how hard it is scraped ({{EXC}}) and how hard the thing is that scrapes ({{HARD}}).",
    "Deep Mod": "Four mod slots on the synth ({{SLT1}}): {{LFO1}} moves the pulse width only as far as the pad is pressed ({{VIA}} {{PRES}}), and pressure itself goes to the cutoff, the cross-modulation and the resonance. Hold a pad and lean: it opens and growls under the finger and lies still without one.",
    "StepShpe": "One held note plays a riff: the custom LFO shape ({{LSHP}}, eight levels) is run once a bar by {{LFO1}} on the pitch at full depth, so the eight levels are eight notes (0 7 12 7 3 10 0 −5 semitones), and by {{LFO2}} on the cutoff. Hold one key; turn a level on {{LSHP}} and that note of the riff moves.",
    "PluckBdy": "A click of the macro oscillator through a string ({{BODY}}). On {{BDY2}}, {{RDAM}} damps the string when the key is let go and {{KDCY}} lets high notes die sooner, as on a real one: play short and long notes, low and high. The strip takes the string's damping away.",
    "DustBody": "Dust through a plate: the body makes its own grains ({{GRAN}} on {{BDY2}}) and they are heard beside the plate they strike ({{DIRE}}). Hold a note. The strip thins the dust out.",
    "ReedBody": "The voice blows a pipe: the oscillator's level grips a tube ({{COUP}} on {{BODY}}) through a reed's curve ({{REED}} on {{BDY2}}), and {{LFO1}} lets the tube wander along {{MRPH}} and back. Hold a note and turn {{COUP}}.",
    "MorphBdy": "The body changes within the note: a click strikes it, and the filter envelope (it moves no cutoff here) carries {{BODY}}'s {{MRPH}} from a string to a bell in a second. Play single notes and hear the partials leave the harmonics. The times on {{F ENV}} are how fast.",
    "FMFilter": "An operator on the filter: operator 6 takes no part in the sound, and its output, at audio rate, is on the cutoff (a slot, {{OP6}} to {{CUT}}). The filter is frequency-modulated at five times the note, and the sidebands die away with the operator's own envelope. Hold a note; the slot's {{AMT}} on {{SLT1}} is how far.",
    "SeqMorph": "The wave sequencer's lanes as sources: the pitch lane ({{PLAN}}) moves {{BODY}}'s {{MRPH}}, so each step of the sequence rings in another body, and the level lane ({{LLAN}}) opens the filter with it. Hold a note and let the sequence run.",
    "Rnd Time": "No two notes alike, and the same again every time: three more randoms a note ({{RND2}} {{RND3}} {{RND4}}) on the envelope's time ({{ETIM}}: some notes are plucks, some swells), the filter envelope's time and the table position. Play the same phrase twice. {{SEED}} on {{ANLG}} chooses another set.",
    "FMWaveOp": "Operator 1 reads the {{BELL}} table instead of a sine ({{WAV1}} on the {{WAVE}} page), and the envelope moves where in it ({{PENV}} on {{WPOS}}). Turn {{POS1}}.",
    "FM Cycle": "Operator 1 reads single cycles of a sample ({{WAV1}} = {{CYCL}}; {{SMPL}} names the sample), modulated by a sine an octave and a fifth up: a hollow, reedy organ. Turn {{SMPL}} for another sample's cycles and {{POS1}} for another place in it.",
    "DualFilt": "Two filters side by side ({{ROUT}} {{PAR}} on {{FLT B}}): a low-pass and, two and a half octaves above it, a resonant band-pass, a second voice in the top of the sound. {{BAL}} is the balance between them, {{OFSB}} moves the upper one.",
    "SerlFilt": "Two filters one after the other ({{ROUT}} {{SER}}): a ladder low-pass into a diode low-pass an octave below it, both resonant. Turn {{CUT}}: two peaks sweep together.",
    "Seeded": "Every note a little off in pitch, cutoff, envelope time and timbre (the {{ANLG}} page), and the same notes come out the same again. Play a phrase twice; turn {{SEED}} for another set of differences.",
    "SeedTabl": "The same seeded spread on the wavetable engine, as a pad: every note of a chord a little different, and the same chord the same again.",
    "Mono Wt": "{{VOX}} 1 (the {{VOICE}} page): one voice, which a new note takes over with a glide. Play a legato line; raise {{VOX}} and the same sound plays chords.",
    "Wide Pad": "{{VOX}} 16: sixteen notes ring at once. Play a chord with both hands and another over its tail: nothing is cut off.",
    "SprngKey": "An electric piano through a spring reverb that is stored with the sound (insert {{SPRG}}). Play short chords; {{DISP}} on the insert's page is the spring's drip.",
    "ShimrPad": "A soft pad into the shimmer reverb (insert {{SHIM}}): the tail climbs an octave with every turn round its loop. Play a chord and let go.",
    "ThruFlng": "A flanger that goes through zero (insert {{FLG}} with {{THRU}} on): the dry sound is delayed by the flanger's centre, so the swept copy passes through it and the two cancel completely once a cycle. Hold a chord; switch {{THRU}} off on {{FX2}} for the ordinary flanger.",
    "PointKit": "A kit in which every pad is a point of one of two machines: three kicks of the kick machine, everything else a sound of the resonator drum. Two pads carry modulation of their own: the gong (top pad 5) has its own LFO on its level and beats five times a second, and the low kick (bottom pad 1) is tuned higher by a harder stroke. Select either on {{DRUMS}} and page on to {{PLFO}} or {{PSLT}}.",
}


def show_table(params):
    by_name = {p["name"]: p for p in params["presets"]}
    missing = [n for n in SHOW_TEXT if n not in by_name]
    if missing:
        raise SystemExit("chapters.py describes presets that are not built in: " + ", ".join(missing))
    rows = [(html.escape(n), f"<code>{html.escape(by_name[n]['category'])}</code><br>{PRESET_ENGINES[by_name[n]['engine']]}", fmt(text))
            for n, text in SHOW_TEXT.items()]
    return table(["Preset", "Where", "What it shows, what to play, what to move"], rows, "cols3")


def router_sources(params):
    lists = [choices(params, engine, "SLT1", "SRC") for _, engine in ROUTED]
    n = 0
    while all(len(l) > n and l[n] == lists[0][n] for l in lists):
        n += 1
    own = {short: l[n:] for (short, _), l in zip(ROUTED, lists)}
    if not own or set.intersection(*(set(o) for o in own.values())):
        raise SystemExit(f"the engines' own sources share a name: the router's list was cut short at {n}")
    return lists[0][:n], own


def own_sources(params, short):
    return " ".join(f"<code>{html.escape(c)}</code>" for c in router_sources(params)[1][short])


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
with eight models, wavetable, wave sequencer, sampler with slicing, time stretch and a granular mode, a sixteen-pad
drum kit, and a resonator (a body and something that sets it ringing: strings, tubes, plates, bells, gongs, struck,
bowed, blown or brushed). A ninth kind of track sends MIDI to outside gear.</li>
<li><b>Five filter models</b> with a live picture of the response curve.</li>
<li><b>Modulation</b>: one router on every sound engine. Per-voice LFOs with eleven shapes (two of them drawn by
you) and up to nine trigger modes, sixteen mod slots that join any source to any destination, another track's sound
as a source (its level, its onsets, its spectrum in sixteen bands, or the sound itself, sample by sample), extra
envelopes, pad pressure routed per track and per note, and the cells of the track's effects as destinations.</li>
<li><b>Effects</b>: eight insert slots a track with 33 effect types, two send buses that are each a chain of
effects, and a master EQ, compressor and limiter.</li>
<li><b>Sequencer</b>: 16 patterns, per-track length and speed, parameter locks, trig conditions, micro-timing, live
recording, song mode.</li>
<li><b>An arpeggiator on every track</b>, with variation and mutation, and pressure-driven rolls on the pads.</li>
<li><b>Pads that play more than notes</b>: chords you store by holding them, and effect moves (an echo thrown onto
one hit, a freeze, a stutter, a tape stop) that can be recorded into the pattern.</li>
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
<li>Plug the Launchkey into the Mac. It needs the Neptune firmware (tag NPF), which is already on your unit.</li>
<li>Start <i>Neptune Groovebox</i>. The display shows the {{TRACKS}} screen within a couple of seconds, and
[[Up]], [[Down]], [[>]] and [[Func]] light up white.</li>
<li>The Mac window shows the display and a picture of the Launchkey with every button named. It can be clicked
when the controller is out of reach: pads light as they are pressed and the two strips show where they stand.</li>
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
clockwise. The display answers OK, and the Mac asks for a name: type one and press Return (Escape takes the save
back). Next time the app starts it reopens this project.</p>""")))
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
<div class="try"><b>Three waves, one table</b>On a wavetable track open {{WAVE}}: give key 1 a sample with {{SM}}, press
the second top pad and give key 2 another, then a third. Set {{AL}} to {{TRNS}} or {{VOC}} and sweep {{POS}}. Chapter 5.</div>
<div class="try"><b>Patterns that never repeat</b>Give two tracks different lengths ({{LN}} on {{STEP}}): 16 against
12, say. Then give some steps a condition ({{COND}}: {{PROB}} or {{A:B}}). Chapter 9.</div>
<div class="try"><b>An arp that evolves</b>Press [[Arp]] twice and hold a chord. On the {{VARY}} page raise
{{SPCE}}; on {{MUTA}} switch mutation on and let it run. When you like what it has become, {{>PAT}} writes it into
the pattern. Chapter 10.</div>
<div class="try"><b>Rolls under your finger</b>On the {{ARP}} screen's {{ROLL}} page switch {{ROLL}} on. Now a held
drum pad repeats, faster the harder you press. Chapter 10.</div>
<div class="try"><b>A synth that talks</b>{{TRK FX}}, add a {{TALK}} insert on the {{FX}} page, then raise {{MIX}}, sweep {{VOWL}}, add {{DPTH}}.
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
tags when something is engaged: {{ARP}}, {{LATCH}}, {{ROLL}} (pad rolls are on with their own latch, {{LTCH}} on
the {{ROL2}} page: a pad keeps rolling after you let go), {{PIN}}, {{SPLIT}}. When there is no room for the words
they shrink to letters: A, L, O for the roll, P.</p>
<p><b>Grid screens</b> show eight cells in two rows of four. Knobs 1 to 4 are the top row, 5 to 8 the bottom row.</p>
<p><b>Bar screens</b> show a picture or list with a strip of eight two-letter labels along the bottom, one per knob.
When you turn a knob the name bar spells out its full name and value. A knob that has nothing to act on at the
moment says why there for a second instead of doing nothing: {{SAMPLER ONLY}}, {{NOT ZOOMED}} and {{ONE SLICE}} on
{{WAVE}}, {{ROUTE OFF}} and {{KIT ONE WAY}} on {{AT ROUTE}}, {{FLT B IS OFF}}, {{FILTER IS OFF}} and {{SEM MRPH ONLY}} on the filter pages,
{{SLOT IS EMPTY}} on {{TRK FX}}, {{NOTHING KEPT}} and {{NOTHING TAPPED}} on {{ARP}}, {{SONG IS EMPTY}} and
{{ONE ROW}} on {{SONG}}, {{NO LOCKS}} on a held step, {{SET DEST}} on a drum pad's {{PLFO}} page (on {{PARAM}} and on {{DRUMS}}), {{NO TAKE}} on
{{PERF}}.</p>""") + "</div>"
            + S.svg("PARAM synth", 2, "A grid screen: the synth's OSC page") + "</div>"
            + '<div class="shots">' + S.svg("TRACKS", 1, "A bar screen: TRACKS, VOL page") + S.svg("ARP", 1, "ARP, first page") + "</div>"),
        ("Screens: Shift + pad", fmt("""
<p>Hold [[Shift]] and press a pad. The screens follow the legends printed above and below the pads.</p>""")
            + table(["Pad", "Top row", "Bottom row"], [
                ("1", fmt("{{PARAM}} — the sound engine's pages"), fmt("{{STEP}} — the step sequencer")),
                ("2", fmt("{{TRACKS}} — mixer and engine choice"), fmt("{{DRUMS}} — drum pads")),
                ("3", fmt("{{TRK FX}} — track effects"), fmt("{{CHORD}} — chord pads (the arpeggiator is on the [[Arp]] button)")),
                ("4", fmt("{{STEP}} — the step sequencer"), fmt("{{SCALE}} — scale pads")),
                ("5", fmt("{{PRESET}} — sound presets"), fmt("{{WAVE}} — waveform and resampling")),
                ("6", fmt("{{SONG}} — song rows"), fmt("{{FLTR}} — the filter picture")),
                ("7", fmt("{{SETTINGS}} (also the [[Settings]] button)"), fmt("{{PERF}} — effects played from the pads")),
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
                (fmt("[[Scale]]"), fmt("Scale lock on / off, when the button is let go (the name bar says which, with the scale). Held + [[▲]] / [[▼]]: the previous / next scale page"), fmt("Opens {{SCALE}}"), "—"),
                (fmt("[[Settings]]"), fmt("Opens {{SETTINGS}}"), "—", "Tap tempo"),
                (fmt("[[Play]]"), "Start / stop; stopping also ends recording", "Stop and return to the start", "Clear"),
                (fmt("[[Rec]]"), fmt("Recording on / off, when the button is let go. Held + [[Play]]: start recording; again: free / quantized"), "Fill, while held", "Copy"),
                (fmt("[[&gt;]]"), "Pattern key (hold)", "—", "Paste")], "keys")
            + fmt("""
<h3>Help cards</h3>
<p>Every key that changes what the other controls do while you hold it has help cards: [[Shift]], [[Func]],
[[&gt;]], [[▲]] / [[▼]] and [[Rec]]. Hold one of them by itself. After a third of a second the display becomes a
help card for that key, and stays for as long as you hold it. If you used a knob, pad or button in the two seconds
before, the key is taken for the start of a chord and the card waits two seconds instead, so [[Shift]] + turn in the
middle of editing never covers the value you are setting. While a card is showing, the two knob-page buttons turn to the key's other cards
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
card does not page when you let go, because the card took its place. [[Rec]] is the same: a quick press switches
recording on or off when you let go, a press held until its card showed does nothing more, so you can read the card
without arming anything. [[Play]], [[Settings]] and [[Arp]] act the moment you press them and have no cards, and
[[Scale]] has none either; what they do with [[Shift]] or [[Func]] is on those keys' cards.</p>
<h3>Copy, clear, paste</h3>
<p>[[Func]] + [[Rec]] copies, [[Func]] + [[Play]] clears, [[Func]] + [[&gt;]] pastes. What they act on follows where
you are: a held step if you are holding one ({{STEP}}: the whole step; {{PARAM}}, {{TRK FX}}, {{FLTR}}: its locks);
otherwise the track's steps on {{STEP}}, the whole pattern on {{TRACKS}}, the track's sound on {{PARAM}} and
{{FLTR}}, and the track's effects on {{TRK FX}}. On {{PARAM}} and {{FLTR}} copy and paste take the whole sound
(engine, mixer, effects), and clear resets the engine's own parameters and leaves the mixer and the effects alone, so
the display calls it {{RESET}}. On {{TRK FX}} all three act on the effects only: the insert slots (their types,
switches and values) and the {{ARPLK}} values. Clearing empties the slots and sets the arp locks to off; volume, pan,
sends, length and speed on the {{TRACK}} page are mixing and sequencing and are never touched. On one of a send's
pages ({{S1FX}} to {{S2FX2}}) the three act on that send's chain of effects instead ({{CLEAR SND1 FX}}), and what
was copied from one send can be pasted onto the other. On {{PERF}} they act on the pad on the knobs, with all its
layers. On other screens the three do nothing, and the name bar says so ({{NO CLEAR HERE}}). The name bar confirms
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
insert's {{TYPE}} (a track's or a send's); storing a chord on a pad; a {{PERF}} pad's {{FX}}, {{CLR}} and {{TAKE}};
loading a multi preset; {{LCKS}} clearing a step's locks; {{EVOL}} turned back to the ancestor; {{>PAT}}; and a recording,
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
<p>On {{DRUMS}}, {{CHORD}}, {{SCALE}} or {{PERF}}, hold [[Shift]] and hold that screen's own bottom pad for half a second. {{PIN}} appears
in the name bar and those pads keep playing, with pressure, on every other screen, so you can play pads with one hand
and turn sound knobs with the other. The same gesture unpins, and so does the end of a recording (the pads then
show the steps you recorded).</p>""")),
    ]))

    # =========================================================================================
    scr = []
    scr.append((None, fmt("<p>Every screen, what its knobs show and what its pads do. Short names are explained in Appendix A.</p>")))

    def screen(title, shots, text):
        figs = '<div class="shots">' + "".join(S.svg(n, p, c) for n, p, c in shots) + "</div>"
        scr.append((title, figs + fmt(text)))

    screen("TRACKS — the mixer", [("TRACKS", 1, "VOL"), ("TRACKS", 3, "SND1"), ("TRACKS", 5, "ENG")], """
<p><b>Knobs:</b> knob N belongs to track N. Five pages: {{VOL}}, {{PAN}}, {{SND1}}, {{SND2}}, {{ENG}}. The name
bar says the page and the pattern you are on (<code>VOL P1</code>; the two send pages as <code>S1 P1</code> and
<code>S2 P1</code>), and while a pattern is queued, that: <code>P1&gt;3</code>.</p>
<p><b>Pads:</b> the top row selects a track. The bottom row mutes (green = sounding, red = muted, blue = solo);
[[Func]] + bottom pad solos.</p>
<p><b>Changing a track's engine:</b> on the {{ENG}} page turn the track's knob to choose an engine ({{VA}} the
synth, {{FM}}, {{MO}} macro, {{WT}} wavetable, {{WS}} wave sequencer, {{SP}} sampler, {{DR}} drums, {{MD}} MIDI,
{{RS}} resonator). It is shown boxed as pending and nothing changes yet. Tap that track's top pad (lit amber) to commit. Leaving the page drops the choice.
Changing engine replaces the track's sound in every pattern.</p>""")
    screen("STEP — the step sequencer", [("STEP", 1, "STEP")], """
<p><b>Knobs:</b> {{LN}} track length, {{SP}} track speed, {{QN}} playback quantize, and the tempo on knob 8.</p>
<p><b>A longer track loops what it had:</b> turn {{LN}} up and the track's steps repeat through the new ones, notes
and locks included: 16 steps turned up to 64 are the 16 four times, ready to be varied. One turn of the knob (clicks
no more than a second and a half apart) repeats the track as it was when the turn began. The one exception: a track
that was longer before and still holds steps beyond its length gets those back as they were, not written over. The
length on {{TRK FX}} does the same.</p>
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
once; a quick tap never shows them. With several steps held, the pages show the one nearest the top left and every held step is edited
together: a knob gives all of them the value you see, and a key gives all of them its note.</p>
<p><b>Recorded pressure:</b> a step whose notes were recorded from pads with pressure has a small dark dot at the
lower right of its cell, and one more page, {{PRES}}, one press back from {{TRIG}} (the pages go round, so it is
also the last page). It shows the recorded curves over the note's length and three cells: {{PRES}} sets how much of
the pressure is played (0 to 200 %; 0 plays none and keeps the recording), {{>LCK}} turns it into parameter locks,
{{CLR}} removes it. {{>LCK}} and {{CLR}} need four clicks clockwise.</p>""")
    screen("PARAM — the sound", [("PARAM synth", 1, "Synth, MAIN (the landing page)"), ("PARAM synth", 7, "Synth, FILTR"), ("PARAM FM", 2, "FM, FMMIX")], """
<p><b>Knobs:</b> the pages of the selected track's engine (Chapter 5), opening on {{MAIN}}: cutoff, resonance and
the level envelope, the same on every engine that has a filter. A drum track shows the pages of the pad last
selected on {{DRUMS}}: its {{DR}} page; on a kick {{KICK}} and {{KCK2}}, on a resonator drum {{RSN}}, on an
oscillator bank {{BANK}}; then the pad's own modulation, {{PLFO}} and {{PSLT}}. After them come the kit's pages:
{{WAV1}} and {{WAV2}} (each pad's wave) and the modulation pages every sound engine ends with ({{XIN}}, {{SLT1}},
{{LSHP}}, {{STRIP}}, {{LFO1}}, {{LFO2}}; Chapter 7).</p>
<p><b>Pads:</b> the track's steps, for locks: dim = empty, green = has a note, orange = has locks, white = held. A
bottom-row pad under a knob that has a pressure range is violet and plays that range instead.</p>""")
    screen("TRK FX — track effects", [("TRK FX", 1, "TRACK"), ("TRK FX", 6, "S1FX: the reverb on send bus 1")], """
<p>Five pages of the track, the same on every track whatever its engine:</p>
<ul>
<li>{{TRACK}}: {{VOL PAN SND1 SND2}} | {{BUS1 BUS2 LEN SPD}}. The send cells are labelled by what the bus carries
({{RVB1}}, {{DLY2}}).</li>
<li>{{ARPLK}}: {{GATE VEL RTCH}}, there to be locked per step so the arp changes along the pattern.</li>
<li>{{FX}}: {{SLOT TYPE ON WET}} and the chosen insert's four controls. A track has eight insert slots, each for any
of the 33 effect types (Chapter 8); {{SLOT}} goes as far as the first empty one.</li>
<li>{{FX2}}: the same slot's further controls, under knobs 5 to 8, where its effect has more than four (most have
eight: the table in Chapter 8).</li>
<li>{{IN}}: the track's input, another track's sound: {{SRC}} (OFF or a track), {{LEVL}}, {{MODE}} (OFF, MIX, RING),
{{FATK}} {{FREL}}. It is what the vocoder, a keyed effect, the {{XIN}} page and the mod slots' input sources
(Chapter 7) listen to. {{SRC}} takes no step lock and no pressure range.</li>
</ul>
<p>After them two pages for each send bus: {{S1FX}} and {{S1FX2}} for bus 1, {{S2FX}} and {{S2FX2}} for bus 2. They
are {{FX}} and {{FX2}} again for the effects on that bus: {{SLOT TYPE ON WET}} and the effect's controls. A bus
belongs to every track, so these pages show the same thing whatever track is selected, and their cells take no step
lock and no pressure range.</p>
<p><b>Page keys:</b> [[Func]] + top pad 1 to 8 are {{TRACK}}, {{ARPLK}}, {{FX}}, {{FX2}}, {{IN}}, {{S1FX}},
{{S1FX2}} and {{S2FX}}; {{S2FX2}}, the ninth page, is one press of the lower knob-page button further.</p>
<p><b>Pads:</b> as on {{PARAM}}; every cell of the track's own pages can be locked and given a pressure range.</p>""")
    screen("DRUMS — drum pads", [("DRUMS", 1, "DRUMS: the lower left pad, a kick"), ("DRUMS", 3, "Its KCK2 page"), ("DRUMS", 4, "Its own LFO, PLFO")], """
<p><b>Pads:</b> sixteen drum voices, played with velocity and pressure. One colour per kind of voice; white while it
sounds. Playing a pad selects it.</p>
<p><b>Knobs:</b> the selected pad's own pages, the same ones {{PARAM}} shows for it, on the knob-page buttons
(they go round). Every page has eight knobs, each with a bar above it and two letters under it; turn one and the
name bar says its name and value.</p>
<ul>
<li>{{DR}}, every pad: {{TN DC TO DV SN LV PN TY}} (tune, decay, tone, drive, snap, level, pan, type).</li>
<li>A kick: {{KICK}}, {{SW ST SC SH KN KT HD AC}}, and {{KCK2}}, {{KD LN RS LS LK SA ED PA}}.</li>
<li>A resonator drum: {{RSN}}, {{SN MR XM EN DP WR MT DR}}. An oscillator bank: {{BANK}},
{{MD PG CL EX DP BL CP LF}}.</li>
<li>Every pad's own modulation: {{PLFO}}, {{RT DP SH DS TG SY DV FD}}, and {{PSLT}},
{{S1 D1 A1 V1 S2 D2 A2 V2}} (Chapter 5).</li>
</ul>
<p>The name bar shows the pad and its type on the first page (<code>P9 KSYN</code>) and the pad and the page on
the others (<code>P9 KCK2</code>). Select another pad and the page stays where that pad has one of the kind (a
kick's {{KICK}} becomes a resonator drum's {{RSN}}); otherwise its first page shows. The screen follows the
selected track if it is a drum track, otherwise the first drum track.</p>
<p>The map above the knobs shows the kit: two letters a pad for its type ({{KS}} a kick, {{RS}} a resonator drum,
{{BK}} an oscillator bank, {{SB}} the sub-bass, {{CP}} the clap), the selected pad inverted.</p>""")
    screen("PRESET — sound presets", [("PRESET", 1, "PRESET")], """
<p><b>Knobs:</b> {{PS}} scroll, {{CT}} category, {{EN}} this engine or any, {{AU}} audition, {{LD}} load, {{SV}}
save, {{DL}} delete, {{VL}} track volume. Load, save and delete need the four clicks.</p>
<p><b>The category row</b> under the name bar shows all ten categories at once, the chosen one inverted, each by as
many letters as tell it from the others: <code>A</code> ALL, <code>I</code> INIT, <code>B</code> BASS, <code>PA</code>
PAD, <code>L</code> LEAD, <code>KE</code> KEYS, <code>FX</code>, <code>PE</code> PERC, <code>KI</code> KIT,
<code>M</code> MULT. The name bar says the chosen one in full.</p>
<p><b>Audition:</b> opening the screen changes nothing. As soon as the highlight moves (knob 1, [[▲]] / [[▼]], or a
change of category or engine filter), the highlighted preset takes over the track's sound for listening only, and
the name bar shows AUD. Each preset is heard in a fresh copy of its engine, exactly as it will sound once loaded
(notes still sounding from the previous one end). The track's own sound comes back, untouched, when you leave the
screen by any route or select another track. {{LD}} keeps the highlighted preset instead (the name bar shows LOAD OK): after that nothing is put back.
{{AU}} is the off switch: turned off (AU- in the name bar), the list scrolls without changing the sound, and
{{LD}} still loads. A project saved while you are listening stores the track's own sound.</p>
<p><b>Pads:</b> play the track on the notes of the scale chosen on {{SCALE}} (its page and root; sixteen semitones up from middle C until you choose one, with the root lit), so you can audition without reaching for the keys.
[[▲]] / [[▼]] also scroll the list.</p>
<p><b>Multi presets:</b> the last category, {{MULT}}, lists presets of several connected tracks (a kit and the pad
it ducks, a loop and the vocoder it speaks through) and nothing else, whatever {{EN}} says. A multi is never
auditioned: moving onto one gives the track its own sound back, and the name bar says where the multi would land
(<code>3TRK&gt;T7-9</code>: the earliest empty tracks) or that there is no room (<code>NEED 3 EMPTY</code>). {{LD}}
loads every member, with its starter pattern in the pattern you are on, as one undo step, and selects the track
the keys should play. A track counts as empty while no step of it is on in any pattern, no preset has been loaded onto it, its sound
is its engine's starting sound, and it has no insert effect and no input. See Chapter 12 for the built-in ones and for saving your own.</p>
<p>Your own presets are files in <i>Music / Neptune / Presets</i>.</p>""")
    screen("SETTINGS", [("SETTINGS", 1, "SETUP"), ("SETTINGS", 2, "MASTR"), ("SETTINGS", 3, "PROJ"),
                        ("SETTINGS", 4, "CLICK")], """
<p>Four pages. {{SETUP}}: tempo, swing, quantize, clock source, master volume, pattern, and {{HELP}} (when the
help cards appear). {{MASTR}}: the master's three-band EQ ({{LOW MID MFRQ HIGH}}), compressor ({{THR RATI MAKE}})
and limiter ({{CEIL}}). {{PROJ}}: project slots, {{SLOT LOAD SAVE SVAS}} | {{NEW}} (Chapter 12). {{CLICK}}: the metronome — {{CLCK}} (when it
clicks), {{CLVL}} (how loud) and {{CNT}} (the count-in before a recording); see Recording in the sequencer
chapter. The metronome's settings belong to the instrument, not to a project: loading or starting a project
leaves them as they are, and they are kept between sessions.</p>""")
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
bar). In edit mode tap a pad and press a key to assign its note.</p>
<p><b>Latch:</b> with the key latch on ([[Shift]] + [[Arp]] while the arp is off) a pad you let go keeps sounding,
as a key does, until you start a new chord or switch the latch off: a new pad pressed with no other pad down ends
the latched ones, a pad pressed while another is still down is added.</p>
<p><b>Scale button:</b> tap to toggle the scale lock. Hold [[Scale]] + [[▲]] / [[▼]] to step through scale pages
(wrapping); if the lock was off, it turns on with the first step. The page name shows in the name bar.
[[Shift]] + [[Scale]] opens this screen.</p>""")
    screen("CHORD — chord pads", [("CHORD", 1, "CHORD"), ("CHORD", 3, "SETS: chord sets")], """
<p><b>Storing:</b> hold a chord on the keys <i>physically</i> and press a pad. The chord is on that pad: exactly the
notes you held, each at the level you played it. Whatever the pad had before is replaced (undo brings it back).
Latched notes alone do not count as held: under the latch a pad press plays the chord, so you can keep the
latch going and play chord pads at the same time. While a finger is on a key the name bar names the chord
(<code>Cm7 &gt; PAD</code>) and the empty pads light up; afterwards it says where the chord went
(<code>PAD 5 = Cm7</code>).</p>
<p><b>Playing:</b> press a pad with no physical key held. It plays its chord on the selected track; the harder you hit, the
louder the chord, and pressure reaches every note. {{VL}} decides what becomes of the velocities the keys were
played with: <code>BALANCE</code> (as the screen starts) keeps their proportions under the pad's velocity,
<code>AS KEYS</code> plays every note exactly as hard as its key was played, however the pad is hit, and
<code>PAD</code> plays them all at the pad's velocity. With the key latch on, a pad you let go keeps sounding (and finishes its strum) until you press the next pad;
a pad pressed while another is still down is added to it. Switching the latch off, or stopping the transport,
ends it. Chord pads record into steps, feed the arp when it is on, and
roll, like every other playing pad. They can be pinned (below), so a progression stays under your fingers while
you edit the sound.</p>
<p><b>Strum:</b> the bank buttons turn to the screen's second knob page, <code>STRUM</code>; its five knobs are one
setting for every pad. {{TM}} is the time from a chord's first note to its last, up to a second; at 0 the notes
start together. {{DR}} strokes upward from the lowest note, downward from the highest, or one then the other.
{{HU}} loosens the timing: each stroke a little quicker or slower than the last, its notes unevenly spaced.
{{VR}} loosens the pattern: now and then a stroke goes the other way, or two neighbouring notes change places.
No note is ever left out. {{FL}} makes the later notes of a stroke softer. A pad let go before its stroke is over
leaves the rest unplayed, so hold the pad for a long strum. This strum is the pads' own and needs no arp; while
the track's arp is on, a pad hands its notes to the arp together and the arp's own {{STRM}} applies instead.</p>
<p><b>Display:</b> the sixteen pads as two rows of eight, each with the chord's root over its kind: <code>M</code>
major, <code>m</code> minor, <code>7</code>, <code>M7</code>, <code>m7</code>, <code>o</code> diminished,
<code>h7</code> half-diminished, <code>+</code> augmented, <code>s2</code> / <code>s4</code> suspended,
<code>6</code>, <code>9</code>, <code>5</code> a bare fifth, <code>*</code> anything else (named by its lowest
note). A line under a cell marks an inversion; <code>--</code> is an empty pad. The pad lights are coloured by
root, neighbours on the circle of fifths in neighbouring colours, so a progression reads as colours.</p>
<p><b>Knobs:</b> {{BK}} bank (four banks of sixteen; also [[▲]] / [[▼]]), {{TR}} transpose and {{OC}} octave for
every pad, {{VL}} whether each note keeps the level it was stored with, {{CL}} clear the bank (four clicks). Chords
are saved with the project.</p>
<p><b>Chord sets:</b> the bank buttons turn to a third knob page, <code>SETS</code> (the name bar shows the slot,
<code>SETS C1</code>). Knob 1, {{SL}}, picks a chord-set slot (1–32; its readout marks one that holds a set with
<code>*</code>), knob 3, {{LD}}, loads the slot's chords into the four banks (four clicks; <code>SLOT EMPTY</code>
where there are none), and knob 8, {{SV}}, saves the four banks to that slot (four clicks). Chord sets are
files in <i>Music / Neptune / Chords</i> and are independent of projects: use them to recall favourite voicings
across songs.</p>""")
    screen("PERF — effects played from the pads", [("PERF", 1, "FX: the layer's effect"), ("PERF", 2, "PRESS: what a press moves"), ("PERF", 3, "PRES: what pressure moves")], """
<p>[[Shift]] + bottom pad 7. Each of the sixteen pads holds an effect move, and pressing the pad plays it: an echo
thrown onto one snare hit, a chord frozen and left to climb away, a bar that stutters, the whole mix grinding to a
stop. Pads for different tracks sit side by side, so one hand can work the effects of the whole pattern. A new
project starts with the same eight moves twice: on the whole mix along the top row, and on the selected track
along the bottom row. Every one of them is yours to change: which effects a pad holds, what they act on, and what
your press and your pressure do to them.</p>
<p><b>Banks:</b> four banks of sixteen pads, <code>B1</code> to <code>B4</code>. [[▲]] / [[▼]] switch the bank
(the same gesture as on {{CHORD}}). The name bar shows which bank you are on. A ringing or latched layer on a bank
you leave keeps sounding — switching banks never cuts a tail short.</p>
<p><b>Pads:</b> press to play. The lights show the kind of effect (echoes amber, reverbs blue, the freeze violet,
repeats green, the stop red, phasers and their kin cyan, filters yellow, distortion magenta): dim while waiting,
full under your finger, half while a tail is still ringing. The pad you touched last is the one on the knobs, a
little brighter than the rest: hold it and turn, and you hear what you change. The pads can be pinned, so they
stay under your fingers on every other screen.</p>
<p><b>Layers:</b> a pad holds up to four effects, <code>A</code> to <code>D</code>, and one press plays them all.
The {{LY}} cell on the <code>PRESS</code> knob page (knob 3) chooses which layer the knobs show; the bottom left
of the display shows the four, a letter for each layer that holds an effect, the shown one inverted. To add an
effect to a pad, step to an empty layer and turn {{FX}}; to remove one, turn its {{FX}} back to <code>--</code>.
Each layer has its own target, so one pad can filter the bass, throw the snare into an echo and crush the whole
mix at once.</p>
<p><b>Knob pages.</b> Four of them, and the name bar says the bank and the page: <code>B1 FX</code>,
<code>B1 PRESS</code>, <code>B1 PRES</code>, <code>B1 MORE</code>.</p>
<p><b>Knob page FX:</b> {{TRK}} is what the layer acts on: <code>SEL</code>, the track that is selected when you press;
<code>MSTR</code>, everything; or one track by number. {{FX}} chooses the effect: the pads' own four (below), then
every insert effect of {{TRK FX}}. {{MODE}} is the way a press brings it in. <code>PNCH</code> (punch): the effect
replaces the sound while the pad is down. <code>THRW</code> (throw): the sound is sent into the effect only while
the pad is down, and what the effect makes of it is added and rings on after you let go. Echoes and reverbs can
do both; the knob says so where an effect can do only one. {{WET}} is how much of the effect you get. Knobs 5 to
8 are the effect's own four values.</p>
<p><b>What your press and your pressure do.</b> The next two knob pages have the same layout as the first:
knob 4 belongs to {{WET}} and knobs 5 to 8 to the effect's four values. On <code>PRESS</code> each of those knobs
sets how far the press itself moves that value for as long as the pad is down; on <code>PRES</code>, how far
leaning on the pad moves it. An amount runs from −100 to +100 of the value's range, and 0 leaves the value alone.
Set as many as you like, on every layer: a single finger can open an echo's feedback, darken its tone, and close a
filter on another track, all by different amounts, and the press and the pressure add up on a value that has
both. Nothing you play is written into the pad: let go and every value is back where its knob put it.
<code>PRESS</code> also holds the pad's {{LTCH}} (a press switches the pad on, the next switches it off),
{{QNT}} (a press waits for the next sixteenth, eighth, beat or bar while the sequencer plays, so a repeat or a
stop lands on the grid) and {{LY}} (which of the pad's four layers the knobs show);
<code>PRES</code> holds {{CURV}}, whether pressure answers early
(<code>SOFT</code>) or late (<code>HARD</code>).</p>
<p><b>The pads' own effects.</b> <code>DUB</code> is a dub echo, a tape echo with a hand on its
feedback: every repeat passes a band filter ({{TONE}} places it) and goes onto the tape again. {{DRIV}} is how
hard the tape is hit. A quiet repeat comes back as it went; a hot one comes back squashed, thickened by its
octave and with its treble worn off, and the next one more so, while the pitch drifts a little as a tape's
does. At the top of {{FDBK}} it goes on by itself, howling at the level the tape will hold, until you ease
off, which is why it starts with pressure on {{FDBK}}. <code>FRZ</code> catches the sound for a moment ({{GRAB}}) and holds it
for as long as the pad is down. Let go and it dies away over {{DEC}}; with {{SPRL}} up it spirals away, climbing
octave on octave as it goes. {{SHIM}} is the same climb while you hold, and starts on pressure.
<code>RPT</code> repeats the last slice of the beat ({{DIV}}); it starts with pressure shortening the slices, down
to 1/64, and {{DCAY}} and {{PTCH}} make each repeat quieter or step it up or down in pitch. <code>STOP</code>
is a tape stopping over {{TIME}}; let go and the sound is back.</p>
<p><b>Knob page MORE:</b> {{SYNC}} locks the layer's time or rate to the tempo and {{DIV}} chooses the division;
the effect's own time knob then shows the division too. Knobs 3 to 6 hold what the layer's effect has beyond
its four controls: on the dub echo {{SAT}}, the curve of its tape (Chapter 8, Saturation curves), {{HEAD}} (one to
four playback heads), {{WIDE}} and {{READ}} (the tape read forwards, backwards, shifted or held); on an insert
effect the cells of its {{FX2}} page. {{TAKE}}, four
clicks, clears the recorded pad moves in this pattern (<code>NO TAKE</code> when nothing has been recorded).
{{CLR}}, four clicks, empties the pad, all its layers. Both are undo steps.</p>
<p><b>Recording a take.</b> While the sequencer plays and records ({{REC}} is on), every pad press, pressure change
and release is stored as a take in this pattern. On the next loop the recorded moves play back automatically:
pads fire at the right time, pressure is replayed. A finger on a pad wins over playback — your live touch always
takes priority. Transport stop releases every pad playback holds down. Record over an existing take to add to it;
turn {{TAKE}} on the {{MORE}} page to clear the take and start fresh. With <code>REC QUANT</code> the moves are
written on the nearest sixteenth. A take belongs to the pattern, not to a track, and a move remembers its bank:
one recorded on bank 2 plays its bank-2 pad whichever bank the screen shows.</p>
<p><b>Good to know:</b> a layer on a track acts after that track's fader, so an echo you threw keeps ringing when
you pull the fader down or mute the track, and one on <code>MSTR</code> sits before the master limiter, which
catches a feedback that got away. [[Shift]] + [[Play]] silences everything, latched pads included.
[[Func]] + [[Record]] / [[Play]] / [[&gt;]] copy, clear and paste the pad on the knobs, with all its layers. The
pads, all four banks and the recorded takes are saved with the project.</p>""")
    screen("SONG", [("SONG", 1, "SONG")], """
<p><b>Knobs:</b> {{PA}} pattern, {{RP}} repeats, {{TR}} transpose, {{RW}} row, {{LP}} loop, {{SG}} song on / off,
{{IN}} insert a row, {{DL}} delete the row (four clicks).</p>
<p><b>Pads:</b> pad N selects row N; the pad after the last row adds one. With {{SG}} on, [[Play]] follows the rows;
a row ends when track 1 has wrapped the set number of times.</p>""")
    screen("AT ROUTE — pad pressure", [("AT ROUTE synth", 1, "DEST over DPTH: the synth's starting route"), ("AT ROUTE synth", 2, "BIP")], """
<p>What pad pressure does on the selected track: up to four routes (a drum kit has one, for all its pads; a MIDI
track none, its pressure is sent out). The routes are part of the track's sound: a preset saved with {{SV}}, a
project and an undo step carry them, and a preset you load or audition brings its own (Chapter 7).</p>
<p><b>Page 1</b> (the name bar says {{DEST}}): the top row is the destination of routes 1 to 4, the bottom row,
{{DPTH}}, how far each moves it, −100 to +100. A route with no destination shows <code>---</code> for its depth,
and that knob says {{ROUTE OFF}}; a destination you choose starts at a depth of 50.</p>
<p><b>Page 2</b> (the knob-page buttons; the name bar says {{BIP}}): the top row switches a route to both ways,
so that half pressure is the middle, less lowers and more raises. The bottom knobs do nothing here.</p>
<p>{{DEST}} goes through its list in this order ([[Func]] + turn jumps from group to group): {{OFF}}; what only
pressure reaches on this engine; everything an LFO of the engine can move; the cells of the effects on the
track. The pads play the track chromatically from middle C so you can feel the result, and the strip at the
bottom shows the pressure.</p>""")
    screen("WAVE — waveform and resampling", [("WAVE", 1, "WAVE"), ("WAVE", 2, "REC")], """
<p>{{WAVE}} page: the track's sample with start and end brackets, loop marks, slice ticks and a moving line for each
sounding voice (in granular mode, a tick per grain). Knobs: {{ST EN LS LE}} markers, {{ZM}} zoom, {{SC}} scroll,
{{MD}} play mode, {{SL}} slices. Pad N selects slice N.</p>
<p>On a wavetable track the page is the key-frame editor of the {{KEYS}} table (Chapter 5): the sample of one key,
a line where its cycle is taken and a tick for every other key on the same sample. Knobs: {{KY}} key, {{SM}} its
sample, {{PO}} its position in the sample, {{PL}} its place along the table, {{ZM}} zoom, {{SC}} scroll, {{AL}}
algorithm, {{TB}} table. The top-row pads choose the key; the lit ones are in use.</p>
<p>{{REC}} page: resampling (Chapter 11).</p>""")
    ch.append(("The screens", scr))

    # =========================================================================================
    eng = []
    eng.append((None, fmt("""
<p>Each track has one engine, chosen on {{TRACKS}} → {{ENG}}. Its pages appear on {{PARAM}}. All engines except
drums and MIDI have a filter (Chapter 6), and every sound engine ends with the same modulation pages: the track
input, the mod slots, the custom LFO shape, the mod strip's routes and the LFOs (Chapter 7).</p>
<h3>The landing page: MAIN</h3>
<p>{{PARAM}} opens on the same page for every engine that has a filter, so the first handles of a sound are always
under the same knobs: knob 1 is the filter cutoff ({{CUT}}), knob 2 the resonance ({{RESO}}), knob 3 how far the
filter envelope moves the cutoff ({{ENV}}; on FM, macro, wavetable, wave sequencer, resonator and synth; the sampler
shows {{ENVF}}), knob 4 the filter {{TYPE}}, and knobs 5 to 8 are the envelope that shapes the level: {{ATK}}, {{DEC}},
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
filter models, the same envelopes, the same modulation (one router: its LFOs, slots, sources and the way a
destination is moved are the same on every engine), the same effects and the same sequencer. Some pieces are one
thing used in several places: the wavetable engine's tables are also the waves of the synth's {{WT}} oscillators,
of FM's operators and of the drums' oscillators; the resonator's body is also the {{BODY}} page of the macro
oscillator and the wave sequencer, the resonator drum of the kit and the vocoder's {{TUNE}} mode.</p>
<p>There is also a bridge between engines: resampling. Any track (a macro oscillator, an FM sound, the synth) can be
recorded on the {{WAVE}} screen's {{REC}} page, and the recording becomes a sample for a sampler track or the
{{USER}} table of a wavetable track (Chapter 11). That is how a macro-oscillator sound becomes a wavetable. To
layer two engines on one note, put them on two tracks and play or sequence both.</p>""")))
    eng.append(("Synth — the configurable voice", fmt("""
<p>Three oscillators and noise into two filters and an amplifier, up to sixteen voices. {{ARCH}} on the {{VOICE}}
page loads a starting point (mono, hybrid, poly) into the same pages; the engine remembers the tweaks you made to
each preset, so switching away and back restores your edits (saved with the project). {{MODE}} sets mono, duo, poly or unison;
{{VOX}} how many notes can sound at once (1 to 16); {{STCK}} stacks detuned copies. {{PW}} shapes every waveform. At 50 each wave is plain, and the two
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
        ("<code>WT</code>", "<code>POS</code> position in the table", "<code>TBL</code> table: SAW SQR TRI SIN PWM FORM HARM SPEC DIGI, then VOWL SYNC FOLD FM1 FM3 BELL ORGN NOIS RESO WLSH PRIM OCTV PDST COMB", "The wavetable engine's oscillator in a slot: any of its 23 built-in tables, read the same way; sweep POS for movement."),
        ("<code>FM</code>", "<code>IDX</code> modulation depth", "<code>RAT</code> modulator ratio, 1–12", "A sine modulated by a second sine at a whole multiple of its pitch: bells, basses, glassy tones."),
        ("<code>FORM</code>", "<code>FRM</code> formant frequency, 1–16 times the pitch", "<code>WID</code> width of the formant", "A resonant, vocal peak that stays put while the note changes character around it."),
        ("<code>HARM</code>", "<code>TLT</code> tilt, bright to dark", "<code>ODD</code> all harmonics to odd only", "Up to twelve harmonics added together; odd-only is hollow like a square."),
    ]) + fmt("""
<p>Whatever moves {{PW}} (an LFO with {{DEST}} {{PW}}, pad pressure routed to {{PW}}) also moves the first control
of every non-analog slot, so one LFO can sweep a pulse's width and a wavetable's position together. Choosing a new
kind for a slot starts it from that kind's own settings. The wavetable kind is the wavetable engine's own
oscillator on the wavetable engine's built-in tables (described in the Wavetable section below), read smoothly.
What stays in the wavetable engine: tables made from your own recordings ({{USER}}, {{KEYS}}), the timed
{{SCAN}}, and the coarse read-outs of {{INTP}}. An LFO sent to a slot's table cell moves it by eight tables at
full depth.</p>""") + page_list(params, "Unified Voice") + fmt("""
<p>The last four pages of the table are the modulation router's, as on every engine: {{XIN}} (another track's
sound on two destinations, among them {{PM}}, {{RING}} and {{SYNC}}: phase modulation of oscillator 1, the mixed
oscillators multiplied by the input, oscillator 1 restarted by it), {{SLT1}} and on (the mod slots), {{LSHP}} (the
custom LFO shape) and {{STRIP}} (what the mod strip does). Three more pages follow that are the synth's own:
{{MOD}} and {{MOD2}} (up to four LFOs) and {{MENV}} (up to four extra envelopes). See Chapter 7.</p>
<p>The {{ANALG}} page is the instrument's "age" control: drift, tuning slop, and per-note variation of waveform,
filter and envelope. With everything at zero, successive notes are identical; raise them and no two are.</p>""")))
    eng.append(("FM — six operators", fmt("""
<p>Six sine operators, eight algorithms. {{FMMIX}} puts all six operator levels under knobs 1 to 6 so you can play
the timbre directly; {{RATIO}} does the same for their frequency ratios. Each operator then has its own page with
ratio, level, detune, feedback and envelope.</p>
<p>The preset <i>Anthem Bell</i> is the struck tubular bell of 1980s film themes, built the way six-operator FM
made it: algorithm 3, three pairs side by side. Two pairs are the bell (a modulator at 3.5 times its carrier,
one pair tuned slightly flat, one slightly sharp, so the tone beats as it rings for seconds); the third is the
strike, at an unrelated ratio with feedback, gone in a third of a second.</p>
<p>{{WAVE}} and {{WPOS}} say what each operator reads: {{SINE}} is the classic operator; {{CYCL}} reads single cycles
of the sample {{SMPL}} names; the rest are the wavetable engine's built-in tables, read where {{POS1}} to {{POS6}}
say. {{WSEQ}} turns every table into a sequence of 2 to 16 of its waves, crossfaded by {{XFAD}}; {{PENV}} lets each
operator's own envelope move its position.</p>
<p><b>Free connections.</b> The algorithm is fixed, but the mod slots (Chapter 7) can add any connection it does
not have. Each operator's output is a source, {{OP1}} to {{OP6}}, sample by sample, and each operator's phase is a
destination, {{PM1}} to {{PM6}}, with {{PMAL}} for all six at once. A slot {{OP3}} → {{PM1}} makes operator 3 a
modulator of operator 1 on top of the algorithm; {{OP1}} → {{PM3}} with {{OP3}} → {{PM1}} is a loop no algorithm
has. These connections are heard one sub-block late (16 samples at 48 kHz): on a steady tone a turn of phase, not a
change of pitch. An operator stays heard or not as the algorithm says: for one that should only modulate through a
slot, choose an algorithm where it is a modulator already, and set the level of the carrier it feeds to 0 on
{{FMMIX}} if that carrier should not be heard. {{OP1}} to {{OP6}} also reach
every other cell: {{OP2}} → {{CUT}} is filter FM by one operator, {{OP4}} → {{POS2}} a table scanned by an
operator, {{OP5}} → {{PAN}} spreads the partials.</p>
<p><b>The track input on an operator.</b> The {{XIN}} page with {{DST1}} on {{PM1}} to {{PMAL}} makes another
track's sound a seventh modulator: at {{AMT1}} 100 a full-level input is as deep as an operator at full level.
(This replaces the old {{XIN}} and {{XDPT}} cells; a sound saved with them loads onto the {{XIN}} page.) {{INDX}}
scales it with the rest of the modulation, and {{ETIM}} stretches or shortens every operator's envelope together.</p>
<p>{{VOX}} on the {{VOICE}} page sets how many notes can sound at once, 1 to 16 (default 8). At 1 the engine is
mono with last-note priority. Lowering {{VOX}} while notes are held releases the excess voices (their release
envelopes run); it does not cut them.</p>""") + page_list(params, "FM Synth")
        + "<p>Algorithms: " + " · ".join(f"<code>{html.escape(c)}</code>" for c in choices(params, "FM Synth", "FMMIX", "ALGO")) + ".</p>"))
    eng.append(("Macro — eight models, four knobs", fmt("""
<p>One oscillator that can be eight different things. {{MODL}} chooses the model, and knobs 2 to 5 are renamed to
suit it.</p>""") + table(["Model", "What it is", "Knobs 2–5"],
                          [(f"<code>{m}</code>", d, " ".join(f"<code>{x}</code>" for x in k.split())) for m, d, k in ctx["macro_models"]])
        + fmt("""<p>Four built-in presets for each model show where its knobs lead (on {{PRESET}} set {{EN}} to this engine):
VA - Super Saw Lead, Hollow Sq Bass, Soft Sine Pad, Octave Strings. FM - FM Glass Keys, FM Growl Bass, Detuned
Chime, Feedback Scream. FOLD - Fold Lead, West Coast Bass, Cheby Organ, Fold Sweep Pad. FORM - Choir Aah, Talking
Lead, Whisper Wind, Vox Bass. HARM - Pure Organ, Stiff Bell, Clarinet Tone, Glass Harmonics. PLCK - Nylon Guitar,
Pluck Bass, Koto Snap, String Drum (and Resonator Pluck). DUST - Vinyl Crackle, Pitched Rain, Hiss Riser, Dust Hat
(and Grain Cloud). BOW - Solo Cello, Glass Bow, Scrape FX, Bowed Bass. Load one, look at knobs 2 to 5 on the first
page, and turn them: each preset sits at one corner of its model.</p>
<p>The model's own output, before the filter, is a source of the mod slots: {{OSC}}, sample by sample (heard one
sub-block, 16 samples at 48 kHz, late). {{OSC}} → {{CUT}} is filter FM by the oscillator itself; {{OSC}} →
{{AMP}} multiplies the sound by itself and brings out the octave.</p>
<p>{{VOX}} on the {{VOICE}} page sets how many notes can sound at once, 1 to 16 (default 8). At 1 the engine is
mono with last-note priority. Lowering {{VOX}} releases excess voices gracefully.</p>
<p>{{BODY}} and {{BDY2}} put a resonating body after the oscillator (see <i>A body after the voice</i>, below).
{{ANLG}} is the per-note spread: {{SLOP}}, {{FVAR}}, {{EVAR}}, {{WVAR}}, {{PHAS}}, {{PVAR}} and {{LVAR}} make each
note a little different in tuning, filter, envelope, wave, start phase, place and level, and {{SEED}} makes the
differences repeat.</p>""")
        + page_list(params, "Macro Oscillator")))
    eng.append(("Wavetable", fmt("""
<p>{{TABL}} picks a built-in table, {{USER}}, a table built from any sample (64 frames, so {{POS}} scans
through the recording), or {{KEYS}}, a table you define with key frames (below). {{MSPD}} and {{MDPT}} move the
position by themselves; {{ENVP}} and {{VELP}} move it with the envelope and velocity. {{INTP}} sets how the table
is read, from smooth to deliberately coarse. Its last setting, {{CLSC}}, reads any table the way the first digital
wavetable synths did: the scan moves in 64 whole steps, the wave is 128 samples of 8 bits with nothing smoothed
in between, and nothing is band-limited to the note, so high notes get a glassy top of tones that are not
harmonics. ({{STEP}} is stepped but clean; {{VINT}} softens each step.)</p>
<p>After the first nine tables come fourteen more: {{VOWL}} the vowels A E I O U, {{SYNC}} an oscillator-sync
sweep, {{FOLD}} a sine driven into a wavefolder, {{FM1}} and {{FM3}} FM with a rising index (modulator at 1× and
3× the pitch), {{BELL}} bar-like partials from a soft to a hard strike, {{ORGN}} an organ pulled out drawbar by
drawbar, {{NOIS}} noise settling into a tone, {{RESO}} a resonant filter sweep, {{WLSH}} square-cornered digital
waves, {{PRIM}} prime-numbered harmonics, {{OCTV}} octaves only, {{PDST}} phase distortion from sine to saw, and
{{COMB}} a saw against a sliding copy of itself. All are level-matched and free of aliasing in themselves.</p>
<p>Each of the fourteen has at least one factory preset on {{PRESET}} (Vowel Lead, Sync Sweep Lead, Fold Bass,
FM Table Keys, Glass Pluck, Struck Bar, Drawbar Organ, Noise To Tone, Reso Sweep Bass, Walsh Stepper, Prime
Shimmer, Octave Climb, Phase Bass, Comb Phase Pad and others), and three play a table through {{CLSC}}: Eight
Bit Vowels, Glassy Bell and Raw Sync Lead. One of the vowel presets speaks: hold a key on <i>I You We
Yeah</i> and the {{SCAN}} page's {{PING}} run walks the vowels there and back, which comes out as those four words,
over and over (A to I is "I", I to U is "you", and back again "we" and "yeah"); it sounds most like a voice around
the A in the middle of the keys. {{TIME}} on {{SCAN}} is how fast it talks (set LFO 1's rate to two divided by it,
so the words keep their shape). The same 23 built-in tables can be played by the synth's oscillator
slots (kind {{WT}}).</p>
<p>The {{SCAN}} page moves the position by itself over a set time, whatever note you play: this is time stretch
for a wavetable. {{MODE}} runs once and holds ({{RUN}}), loops, goes there and back ({{PING}}), moves in audible
steps ({{STEP}}) or jumps to random frames ({{RAND}}). The pass lasts {{TIME}} seconds, or with {{SYNC}} on, {{LEN}}
at the tempo; with {{SYNC}} = {{NAT}} a USER table plays back at the original recording's length, and {{SPD}} then
slows it or speeds it without changing pitch. The scan starts from {{POS}}.</p>
<p>{{FRMS}} chooses how a recording is cut into the USER table. The {{A64}}, {{A128}} and {{A256}} settings align
each frame to the sound's pitch period: scanning is far smoother and the table plays in tune with the keys.
They work on pitched material; noise and silence are cut the plain way.</p>""")
        + fmt("""
<h3>Key frames: your own table</h3>
<p>The {{KEYS}} table is made from up to eight single waves, the key frames, each picked out of a sample. You say
where each one sits along the table, and Neptune computes the frames in between.</p>
<ol>
<li>On the wavetable track open {{WAVE}} ([[Shift]] + bottom pad 5). On this track its first page is the key
editor. {{KY}}, or a top-row pad, chooses one of the eight keys.</li>
<li>{{SM}} gives the key a sample: a factory sample, a file, or a recording made on the {{REC}} page (a take made
while the table is {{KEYS}} goes to the chosen key). The table switches to {{KEYS}} and you hear the key at once.</li>
<li>{{PO}} moves the line on the waveform: one cycle is cut there. Where the sample has a pitch the cycle is
exactly one period, so the wave plays in tune with the keys whatever the pitch of the recording; in noise or a
drum hit a short piece is taken. {{ZM}} and {{SC}} zoom and scroll for a finer aim.</li>
<li>Do the same for a second and a third key, from the same sample or from others. {{PL}} sets where each sits
along the table; only their order and spacing count, the earliest key is the start of the table and the latest
the end.</li>
<li>{{AL}} chooses how the frames between two keys are made. Sweep {{POS}} (or use the {{SCAN}} page) and listen.</li>
</ol>
<p>The same settings are cells on four pages of the track: {{K SMP}}, {{K POS}} and {{K PLC}} hold the sample,
position and place of the eight keys, one knob per key, and {{KEYS}} holds the rest.</p>""")
        + table(["<code>ALGO</code>", "Between two keys"], [
            ("<code>LIN</code>", "A plain crossfade. Harmonics the two keys hold in opposite phase pass through silence."),
            ("<code>STEP</code>", "Nothing in between: each key holds until the next takes over."),
            ("<code>CLSC</code>", fmt("A crossfade stored as an early digital instrument stored waves: 128 samples of 8 bits. With {{INTP}} on {{CLSC}} and a low {{KFRM}} this is the whole classic character.")),
            ("<code>PHAS</code>", "Each harmonic's level and phase move separately, so nothing cancels on the way."),
            ("<code>TRNS</code>", "Optimal transport: the energy of the wave glides along the harmonics. A formant or a single partial slides from where it is in one key to where it is in the next, instead of fading out in one place and in at another."),
            ("<code>VOC</code>", "Vocoder style: first the outline of the spectrum (its formants) changes, then the harmonics under it. Half way you hear one key's formants on the other key's harmonics."),
        ])
        + fmt("""
<p>{{KFRM}} sets the number of frames (8 to 256), {{BIAS}} leans the in-between frames towards the earlier or the
later key, and {{WRAP}} brings the table back to its first key at the end, for a looped scan without a jump.
{{ALGN}} lines the keys up in phase (leave it on unless you want the raw cuts), {{NORM}} evens out the level of the
frames, {{TILT}} darkens or brightens every key and {{HARM}} removes the harmonics above a number. Each change
rebuilds the table, so these cells cannot be locked to steps. Key frames are saved with the project; their
samples are found again by name.</p>
<h3>The track input and the table</h3>
<p>The {{XIN}} page plays another track's sound into the oscillator, sample by sample. {{DST1}} = {{PM}} is phase
modulation: the input is added to the place the table is read at, up to two cycles either way (the amount counts
squared), so the other track's sound becomes sidebands around every harmonic of the note. {{DST1}} = {{POS}} moves
the table position around where it stands, up to the whole table either way; past an end it comes back in from that
end. On this engine {{LPF}} and {{HPF}} start open (20 kHz and 20 Hz).</p>
<p>The engine adds three sources of its own to the slots: {{SCAN}}, where the timed scan of the {{SCAN}} page is
(0 at {{POS}}, 1 at the end of the table, 0 while {{MODE}} is {{OFF}}: {{SCAN}} → {{CUT}} opens the filter as the
table is run through); {{XFLT}}, the track input through two-pole filters at the {{XIN}} page's {{HPF}} and {{LPF}},
twice as steep as the router's own {{XLP}} and {{XHP}}; and {{XRCT}}, that input rectified (every swing made
positive: an octave up, and an envelope of the input). Both input sources reach the oscillator with no delay. The
old {{XIN}} page's cells are slots now. Put the input on a free slot (say slot 1: {{SRC}} {{XIN}}, {{DEST}}
{{PM}}); {{XENV}} is then a second slot {{ENV}} → {{AMT1}} (or simply {{VIA}} {{ENV}} on the first), {{XVEL}} the
same with {{VEL}}, {{XWHL}} with {{STRP}}; {{XLP}} / {{XHP}} are the source {{XFLT}} and {{XRCT}} the source of that
name. A sound saved with the old cells does not bring its input routing back: set it again.</p>
<p>{{SEED}} on the {{ANLG}} page repeats the per-note spread, the random scan ({{MODE}} = {{RAND}}) and the slots'
{{RAND}} and {{RND2}} to {{RND4}} together.</p>
<p>{{VOX}} on the {{VOICE}} page sets how many notes can sound at once, 1 to 16 (default 8). At 1 the engine is
mono with last-note priority; with {{GLID}} above zero the pitch glides to the new note. Lowering {{VOX}} releases
excess voices gracefully.</p>""")
        + page_list(params, "Wavetable")
        + "<p>Tables: " + " · ".join(f"<code>{c}</code>" for c in choices(params, "Wavetable", "WAVE", "TABL")) + ".</p>"))
    eng.append(("Wave sequencer", fmt("""
<p>Each note steps through a short sequence of waves. Pitch, shape, level and step time each have their own lane of
up to sixteen steps, and the lanes can be different lengths ({{TLEN PLEN SLEN LLEN}}), so the combination drifts
before repeating. On the lane pages, knob N is step N.</p>
<p>{{LOOP}} sets how the lanes go round: {{FWD}}, {{REV}}, {{PING}}, {{ONCE}}, {{RAND}} (any step of the lane, drawn
for each note: every voice of a chord walks its own way, and the same notes in the same order walk the same way
again) and {{ORND}}, the random loop as it was before 22 November 2026. That one is not much of a random walk: on
lanes of 2, 4, 8 or 16 steps each lane goes round a short cycle (4 of 16 steps, or 2 of 8), and every voice of a
chord follows the same one. It is kept so that an older sound can be heard as it was; a sound saved with {{RAND}}
now plays the new walk.</p>
<p><b>The lanes as sources.</b> The four lanes are sources of the mod slots (Chapter 7): {{PLAN}} the pitch lane's
step (±24 semitones as −100 to +100, before {{PRNG}}), {{SLAN}} the shape lane ({{SIN SAW SQR TRI PLS NOIS}} as 0,
20 … 100), {{LLAN}} the level lane, {{TLAN}} the step time (30 ms to 2 s, by octaves). So a lane can step the
filter ({{LLAN}} → {{CUT}}), the body, an LFO's rate or another lane's steps, each at its own length. With
{{SLID}} on, {{PLAN}} and {{LLAN}} glide as the sound does. The phase destination {{PM}} (a cycle either way) takes
the track input from the {{XIN}} page as phase modulation.</p>
<p>{{VOX}} on the {{VOICE}} page sets how many notes can sound at once, 1 to 16 (default 8). At 1 the engine is
mono with last-note priority. Lowering {{VOX}} releases excess voices gracefully.</p>
<p>{{BODY}} and {{BDY2}} put a resonating body after the sequence (next section).</p>""") + page_list(params, "Wave Sequencer")))
    eng.append(("A body after the voice: BODY and BDY2", fmt("""
<p>The macro oscillator and the wave sequencer can play through a body: the resonator engine's own, set ringing by
the voice instead of by an exciter. A string, a tube, a plate or a bell then sounds in sympathy with whatever the
voice does, at the pitch of the note. With {{MIX}} at 0 (as it starts) there is no body and it costs nothing.</p>
<ul>
<li>{{BODY}} page: {{MIX}} how much of the voice is the body's sound. {{MRPH}} which body, along one line:
{{STRG TUBE COMB PLAT BAR BELL HEAD GONG TAM CRSH RIDE CUP CHNA SPLS}}; a value between two neighbours is a body
between them. {{DCAY}} how long it rings (0.05 to 20 s). {{COUP}} how the voice meets it: at 0 the body is struck
by every sample of the voice and rings like a filter; raised, the voice's level grips it as a bow does, and the
body sings by itself for as long as the voice sounds. {{DAMP}} how much sooner its high partials die, {{POS}} where
it is driven, {{TUNE}} the body against the note (a fifth, an octave, a beating unison) and {{KTRK}} how far it
follows the keys: at 0 one fixed body that every note is played through.</li>
<li>{{BDY2}} page: {{CONT}}, from one burst at the start of the note (the body then rings freely) to fed for as
long as the voice sounds, which is how it starts. {{GRAN}} adds the body's own sparse impulses, up to noise.
{{REED}} bends the grip from a bow's to a reed's and {{HARD}} makes what drives the body duller or brighter.
{{RDAM}} is what happens when the voice stops: 0 rings on for the whole {{DCAY}}, 100 is stopped at once, a palm
mute. {{KDCY}} makes the decay follow the pitch (higher notes shorter, or longer). {{DIRE}} lets the exciter be
heard beside the body and {{WIRE}} lays something against it that rattles: snare wires on a head.</li>
</ul>
<p>All sixteen cells are destinations of the LFOs, the strip and the slots ({{BMIX}} to {{BWIR}} in the lists):
{{ENV}} → {{BMRP}} is a body that changes as the note sounds. Three presets to start from: <i>Pluck Body</i> and
<i>Reed Body</i> (macro), <i>Dust Body</i> (wave sequencer); <i>Morph Body</i> moves the body with an LFO.</p>""")))
    eng.append(("Sampler", fmt("""
<p>Plays a sample from the bank: the built-in loop and hits, WAV or AIFF files you put in <i>Music / Neptune /
Samples</i>, and anything you resample. {{MODE}} sets how it plays; {{GRAN}} switches to granular playback, with its
own page. The {{SLICE}} page cuts the sample into slices, evenly or at its hits, and the {{STRCH}} page changes its
speed without changing its pitch (all in Chapter 11). Edit the markers by eye on the {{WAVE}} screen.</p>
<p>Three sources of the mod slots (Chapter 7) say where the sample is: {{POS}}, the playhead between the start and
the end of the slice or sample (in {{GRAN}} mode and in a stretch, the scan position), so a filter or the pan can
follow the sample along; {{GRN}}, where the grain that started last lies in the slice or sample (the scan with
{{GPOS}} and {{SPRY}} on it); and {{SLEV}}, the sample's own level at the playhead (its loudness over 23 ms, against
its loudest 23 ms): {{SLEV}} → {{CUT}} opens the filter on the loud parts of a loop. The {{XIN}} page puts another
track's sound on {{PTCH}}, {{CUT}}, {{AMP}} (amplitude modulation; ring modulation with a slot {{ONE}} → {{AMP}} at
−100 beside it) or any other destination; {{ETIM}} lengthens or
shortens the envelope's attack, decay and release together. The grains, the random start and {{PLAY}} =
{{RAND}} draw from the per-note generator: each note its own draws, the same again for the same notes in the same
order.</p>
<p>{{VOX}} on the {{AMP}} page sets how many notes can sound at once, 1 to 16 (default 8). A note that would exceed
the limit fades out the quietest released voice (or the oldest held one) over a few milliseconds, so the new note
starts clean.</p>""") + page_list(params, "Sampler")))
    eng.append(("Drums", fmt("""
<p>Sixteen pads, each a small instrument with eight controls of its own. Play and select them on {{DRUMS}}; edit the
selected one there or on {{PARAM}}. A pad's {{TYPE}} says what it is: a kick (four starting points of one kick
machine), a clap, a shaker, a sub-bass, a simple cymbal, a resonator drum (toms, snares, cymbals, bells and gongs)
or an oscillator bank (hats and analog cymbals). Closed and open hats cut each other off, and the open hat is open
for as long as the pad, key or step lasts and closes when it ends: tap it for a closed hat, hold it (or give its
step a longer {{LEN}}) for an open one.</p>""") + page_list(params, "Drum")
        + "<p>Types: " + " · ".join(f"<code>{c}</code>" for c in choices(params, "Drum", "DR 1", "TYPE"))
        + fmt(""" (the kick machine as an FM kick, as an analog kick and as a modelled kick; clap, shaker, sub-bass;
the kick machine itself; cymbal; resonator drum; oscillator bank).</p>
<p>The table lists a pad's pages, then the kit's: one {{DR}} page a pad, the page or pages its type adds, its
{{PLFO}} and {{PSLT}}, and after the sixteen pads {{WAV1}}, {{WAV2}} and the modulation pages.</p>
<p>The five sound cells act on every type. {{DEC}} is the length: a drum is a one-shot and rings for its {{DEC}}
however short the pad press or the step ({{SUB}}, the bass note, is the exception: it sounds while held and fades
over {{DEC}} afterwards). {{TONE}} is brightness in the way of each drum (Appendix A lists them). {{DRIV}} is
saturation. {{SNAP}} is the front edge: 50 leaves the drum as it is, lower softens the onset, higher lifts it and
gives the tonal drums a click. Pad pressure ({{AT ROUTE}}: one route for the kit) reaches {{DEC}}, the pitch,
{{TONE}}, the level, {{DRIV}} and {{PAN}} of the hit that is sounding; {{SNAP}} belongs to the strike, so pressure
on it cannot change the hit that is sounding.</p>
<h3>The kick machine (KSYN, and FMKK, KICK, MKIK)</h3>
<p>The lower left pad of a new kit is a kick machine; set any other pad's {{TYPE}} to {{KSYN}} and it becomes one
too. A kick machine is a full kick synthesizer with two more pages, {{KICK}} and {{KCK2}}, after the pad's own
page on {{PARAM}} and on {{DRUMS}} (where the labels are two letters: {{SW ST SC SH KN KT HD AC}} and
{{KD LN RS LS LK SA ED PA}}).</p>
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
<li>{{KCK2}}, every cell at 0 leaving the kick as the first page made it: {{KIND}} turns the knock from a tick
into frequency modulation of the body ({{KNCK}} is then its depth, {{KTON}} the modulator's ratio); {{LIN}} lets
the sweep fall evenly in Hz instead of in pitch; {{RES}} gives the low-pass a resonance, {{LPSW}} opens it with
the pitch sweep and {{LPKT}} measures it from the kick's own pitch instead of from 200 Hz; {{SAT}} saturates the
body before its level envelope; {{EDGE}} is the front edge as {{SNAP}} makes it on the other drums (softened below
0, lifted with a noise click above); {{PATH}} chooses the order of the stages, the kick machine's own
(<code>KSYN</code>) or the older kicks' (<code>DRUM</code>).</li>
</ul>
<p>{{FMKK}}, {{KICK}} and {{MKIK}} are the same machine at three other starting points: an FM kick, an analog kick
and a modelled kick. Turning {{TYPE}} to one of them loads that kick into the {{KICK}} and {{KCK2}} pages and
leaves the pad's own page alone; from there it is yours to change like any kick. Every cell of both pages can be
locked per step and given a pressure range; the cells of {{KCK2}} are read when the drum is struck.</p>
<h3>The resonator drum (RSN)</h3>
<p>Set a pad's {{TYPE}} to {{RSN}} and it becomes a resonator drum: toms, snares, hats, cymbals, bells, gongs and
anything else the resonator engine can make, as a one-shot drum with its own pitch and decay. Its second page,
{{RSN}}, holds {{SND}}, the sound (""") + " ".join(f"<code>{c}</code>" for c in choices(params, "Drum", "RSN", "SND")) + fmt("""),
and seven cells that add to the sound's own values, 0 leaving it as it is: {{MRPH}} moves its body along the
resonator's line of bodies, {{XMPH}} its exciter (strike, bow, breath, scrape, brush), then {{ENRG}}, {{DAMP}},
{{WIRE}}, {{MEET}}, {{DIR}}. A stroke adds to what is still ringing. The same sounds are presets of the resonator
engine, for playing them on the keys.</p>
<h3>The oscillator bank (BANK)</h3>
<p>Set a pad's {{TYPE}} to {{BANK}} for the oscillator bank: six square oscillators at unrelated pitches through
two bands, the classic drum-machine recipe for hats and cymbals. Its second page, {{BANK}}, has {{MODL}}
({{HAT}}, {{OHAT}}, {{METL}}, {{RING}}), {{PING}}, {{COLR}}, {{EXC}}, {{DAMP}}, {{BLOM}}, {{COUP}} and {{LOFI}}.
{{HAT}} and {{OHAT}} are the hats: the open hat is open for as long as the pad is held. {{METL}} is the analog
cymbal and {{RING}} the same with the squares multiplied in pairs. {{TUNE}} is the size of the instrument and
{{TONE}} the level of its top.</p>
<h3>Each pad's wave: WAV1 and WAV2</h3>
<p>The drums that have an oscillator of their own (the kicks' body, {{SUB}}) can read something other than a sine:
{{W1}} to {{W8}} on {{WAV1}} are the top row's pads, {{W9}} to {{W16}} on {{WAV2}} the bottom row's, each a choice
of {{SINE}} or one of the wavetable engine's built-in tables. Where in the table the drum reads is the destination
{{WPOS}}: an envelope or an LFO sent there moves a kick through a table as it sounds, and {{PM}} is the phase of
the same oscillator. (The other engines' wave lists have {{CYCL}}, the cycles of a sample, after {{SINE}}; a drum has
no sample to read, so a pad's list goes from {{SINE}} straight to the tables. A kit saved with {{CYCL}} on a pad
played the sine and loads as {{SINE}}.)</p>
<h3>A pad's own modulation: PLFO and PSLT</h3>
<p>The kit's two LFOs, its sixteen slots and its {{STRIP}} page belong to the whole kit: every drum runs them, each
with its own values. On top of that every pad has an LFO and two slots that act on that pad alone, on the two
pages after its sound pages on {{PARAM}} and on {{DRUMS}}: {{PLFO}} ({{RATE DPTH SHPE DEST}} | {{TRIG SYNC DIV FADE}}) and {{PSLT}}
({{SRC1 DST1 AMT1 VIA1}} | {{SRC2 DST2 AMT2 VIA2}}). They offer the kit's destinations and sources and add to
whatever the kit's modulation does to the same thing: an LFO on the pitch of one tom, the velocity of the snare on
its {{TONE}}, the decay of the hat following the strip. A {{DPTH}} or {{AMT}} turned while its destination is
{{OFF}} says so ({{SET DEST}}). A pad's modulation is worked out every 16 samples.</p>
<p><b>Ready-made kits.</b> The built-in kits are on {{PRESET}} ([[Shift]] + top pad 5, {{CT}} = KIT). Two of them hold
settings to play and take apart: <i>Cymbal Works</i>, resonator cymbals and bank sounds across the pads, and
<i>House Kit</i>, a kit to play at once: kick machine, crashes, rides, snare, clap and hats. A new project's kit
has three kicks, a resonator tom and snare, a clap and the bank's two hats along the bottom row, six sub-bass
notes and the resonator's crash and ride along the top.</p>
<h3>Playing the kit</h3>
<p>The pads play the kit on {{DRUMS}} ([[Shift]] + bottom pad 2); on the other screens the pads do that screen's own
job, unless {{DRUMS}} is pinned. The keys play the kit on every screen while a drum track is selected. From the
lowest C, eight keys (C to G) are the bottom pad row from left to right, so the lowest C is the lower left pad; the
next eight (G♯ to D♯) are the top row; the last eight (E to B) are the bottom row once more, and the top C is the
lower left pad again. Harder keys are louder. The layout repeats every two octaves, so after one press of an octave
button the same rows sit an octave along, and after two the keys are as they were.</p>
<div class="note"><b>Sequencing drums</b>Record drum patterns by playing the pads or the keys with [[Rec]] on. On
{{STEP}} you enter one drum at a time: the pads show the hits of the pad last selected on {{DRUMS}}. A tap adds that
drum to a step or takes it away, and the other drums on the step stay, so any number of the sixteen can hit
together. A pad glows dimly, and the step number is underlined, where only other drums hit. Hold a step and press a
key to add that key's drum. On the step's {{TRIG}} page {{NOTE}} and {{VEL}} are the selected drum's own; length,
timing, condition and locks belong to the step.</div>""")
        + '<div class="shots">' + S.svg("PARAM drums", 2, "A kick pad, KICK") + S.svg("PARAM drums", 3, "A kick pad, KCK2")
        + S.svg("PARAM drums", 4, "A pad's own LFO, PLFO") + "</div>"))
    eng.append(("Resonator", fmt("""
<p>A body, and something that sets it ringing. The body is up to 64 partials that ring and die; what a string, a
tube, a plate, a bell or a gong is, here, is where those partials lie and how long each lasts. The exciter is a
strike, a bow, breath, a scrape or a brush. Any exciter can meet any body, and both can be something in between:
that is the whole engine, and it is the same body the kit's resonator drum, the {{BODY}} pages and the vocoder's
{{TUNE}} mode use. Eight voices.</p>
<ul>
<li>{{RSN}}: {{MRPH}} is the body, a place on one line: {{STRG TUBE COMB PLAT BAR BELL HEAD GONG TAM CRSH RIDE CUP CHNA SPLS}} (0 to 13; 2.5 is half way between a comb and a plate). {{XMPH}} is the exciter: {{STRK BOW BRTH SCRP BRSH}} (0 to 4). {{DCAY}} is how long the body rings, {{ENRG}} how hard the exciter drives it and {{VSEN}} how much
a harder note drives it harder; {{TUNE}}, {{FINE}} and {{LEVL}} are what they are elsewhere.</li>
<li>{{BODY}} and {{BOD2}} add to what {{MRPH}} gives, so with every cell at 0 the body is the one {{MRPH}} names.
{{STIF}} stretches the partials as a stiff string or a bar does, {{SHT}} leans towards a plate's dense, irregular
ones, {{SHEL}} towards a bell's, {{ODD}} removes the even ones (a tube closed at one end), {{HUM}} adds an octave
under the note, {{MEMB}} leans towards a drum head. {{DAMP}} and {{TILT}} are how long and how loud the high
partials are against the low, {{POS}} where the body is touched. {{DROP}} lets the pitch fall as the body dies (a
tom's glide), {{WIRE}} lays wires against it, {{PAIR}} makes pairs of partials beat, {{HAND}} leans towards the
partials placed by hand for the gong, the tam-tam and the cymbals, {{MEET}} and {{SPRD}} say which partials a bow
or breath meets on a heavy body, and {{DIR}} lets the exciter itself be heard.</li>
<li>{{EXC}} adds to what {{XMPH}} gives: {{CONT}} from one burst to fed for as long as there is energy, {{GRAN}}
from one impulse through sparse ones to noise, {{COUP}} how much the exciter grips the body (a strike not at all;
a bow or a reed so that the body sings by itself), {{REED}} from a bow's grip to a reed's, {{HARD}} dull to bright.
{{CURV}} is the curve of that grip ({{PHYS}}, or one of the saturation curves), {{IN}} lets the track input (TRK
FX → {{IN}}) excite the body, beside the exciter and at the top instead of it, and {{LAG}} is how slowly the energy
follows your playing.</li>
<li>{{GRAIN}}: give {{SMPL}} a sample and {{GRAN}}'s impulses are grains of it ({{GPOS}} where, {{SCAN}} moving on
through it, {{SPRY}} scattered, {{SIZE}}, {{GPIT}} and {{PJIT}} their length and pitch, {{SHAP}} their window): a
recording played through a body.</li>
<li>{{AMP}}: the envelope. On a struck sound it is the level; on a bowed or blown one it is the bow: the energy
follows it ({{CHAN}} = <code>RING</code> makes the envelope the energy alone and lets the body's own decay be the
level). {{FILTR}} and {{F ENV}} are the shared filter (Chapter 6), open to start with.</li>
</ul>
<p><b>Playing it.</b> Velocity strikes harder or bows deeper. Pad pressure on a new resonator track drives the
exciter (a destination of its own that the pressure routes reach; see Chapter 7): lean on a {{SCALE}} or
{{AT ROUTE}} pad under a bowed sound and the bow digs in. The mod strip starts on {{ENRG}} (+100) with a little
less damping and a vibrato. A new note takes a silent voice while there is one, so tails ring out.</p>
<p><b>Presets.</b> The drum sounds the kit's resonator drum offers are presets of this engine too (Tom, Snare, Hat
Closed, Ride, Crash, Gong, Tam-Tam and their kin, under their own category in the list: set {{CT}} to ALL), with
bowed and brushed ones beside them (Bowed Cymbal, Brushed Snare, Bowed Bar). <i>String To Bell</i>, <i>Shifting
Bodies</i> and <i>Strike To Bow</i> move {{MRPH}} or {{XMPH}} while a note sounds; <i>Bow Cello</i>, <i>Rsn
Glass</i>, <i>Breath Pipe</i> and <i>Scrape Bar</i> are four exciters on four bodies.</p>""")
        + '<div class="shots">' + S.svg("PARAM resonator", 2, "Resonator, RSN") + S.svg("PARAM resonator", 5, "Resonator, EXC") + "</div>"
        + page_list(params, "Resonator")))
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
<p><b>The synth and the wavetable have two filters.</b> The {{FLT B}} page sets the second one: its own type and
mode, its cutoff as an offset from filter A ({{OFSB}}), and {{ROUT}}: single, series or parallel, with {{BAL}} to
balance them. On the wavetable the page also has {{FFM}}, the oscillator modulating the cutoff at audio rate.</p>
<p><b>FM, macro, wavetable, wave sequencer and resonator</b> share one filter design ({{FILTR}} and {{F ENV}} pages),
open by default so presets sound as they did before you touch it. <b>The sampler</b> has its own {{FILTR}} page.
A filter is also available as an insert effect ({{FLT}}) for any track, drums included.</p>""")
            + '<div class="shots">' + S.svg("FLTR", 1, "FLTR") + S.svg("PARAM synth", 8, "Synth, FLT B") + "</div>")]))

    # =========================================================================================
    lfo_shapes = choices(params, "FM Synth", "LFO1", "SHPE")
    dests = []
    for e_name, shown in (("FM Synth", "FM"), ("Macro Oscillator", "Macro"), ("Wavetable", "Wavetable"),
                          ("Wave Sequencer", "Wave seq"), ("Sampler", "Sampler"), ("Drum", "Drums"), ("Resonator", "Resonator")):
        labels = choices(params, e_name, "LFO1", "DEST")[1:]
        # (The cells of the track's effects follow on every engine: one row for all of them, below.)
        if "1:C1" in labels:
            dest_cell(labels)       # (every name is described)
            labels = labels[:labels.index("1:C1")]
        dests.append((shown, dest_cell(labels)))
    dests.append(("Every engine, then", "72: <code>1:C1</code> … <code>1:C8</code> <code>1:WT</code> for each of the eight insert slots "
                  "(<code>8:WT</code> the last): the track's effects (Slots, below). After them eight reserved places that move "
                  "nothing: <code>CH.R</code> <code>CH.D</code> <code>FL.R</code> <code>FL.D</code> <code>PH.R</code> "
                  "<code>PH.D</code> <code>RO.S</code> <code>RO.D</code>"))
    # The synth's STRIP, XIN and slot lists go on past its LFOs' own (the MOD page's): what they add.
    synth_router = choices(params, "Unified Voice", "STRIP", "DST1")[1:]
    synth_router = synth_router[:synth_router.index("1:C1")]
    if synth_router[:len(SYNTH_DESTS.replace("| ", "").split())] != SYNTH_DESTS.replace("| ", "").split():
        raise SystemExit("the synth's destination list no longer begins with SYNTH_DESTS: correct the constant")
    synth_more = synth_router[len(SYNTH_DESTS.replace("| ", "").split()):]
    ch.append(("Modulation and pressure", [
        ("LFOs", fmt("""
<p>LFOs run per voice: each note has its own. The synth has up to four, on its {{MOD}} and {{MOD2}} pages (choose
which with {{LFO}}, how many with {{LFOS}}). Every other sound engine has two, on the last two of the modulation
pages its {{PARAM}} ends with: {{XIN}} (the track input, below), the mod slots {{SLT1}} and on, {{LSHP}} (the custom
LFO shape), {{STRIP}} (what the mod strip does, see Strips below), then {{LFO1}} and {{LFO2}}. The synth has the
same pages up to {{STRIP}}, before its own {{MOD}} page. On a drum kit the two LFOs are the kit's: every drum runs
them, each with its own values, and each pad has one more of its own ({{PLFO}}, Chapter 5).</p>""")
            + table(["Cell", "What it does"], [
                ("<code>RATE</code>", fmt("Speed in Hz. With {{SYNC}} on, a tempo division instead (the {{DIV}} cell; on the synth {{RATE}} itself shows it): 22 values from 16 bars to 1/128, dotted and triplet included; both values are remembered.")),
                ("<code>DPTH</code>", "Amount; negative inverts."),
                ("<code>SHPE</code>", " ".join(f"<code>{s}</code>" for s in lfo_shapes) + fmt(" — sine, triangle, square, saw, stepped random, smooth random, ramp, two exponential shapes that only push one way, and the track's own shape from the {{LSHP}} page: {{USR}} holds each of its eight levels for an eighth of the cycle, {{USRS}} joins them with straight lines.")),
                ("<code>TRIG</code>", fmt("{{FREE}} runs on regardless of notes. {{KEY}} restarts with each note. {{ONE}} plays one cycle and stops: a one-shot envelope in any shape. {{HALF}} plays half a cycle. {{HOLD}} takes one value at note-on and keeps it: a different value per note. On the {{LFO1}} and {{LFO2}} pages four more (the synth's {{MOD}} page has the first five): {{XKEY}}, {{XONE}}, {{XHLF}}, {{XHLD}} are the same four restarted by a hit in the track input (a sound beginning there, as the source {{ONST}} hears it) instead of by a note, in every voice at once. A note does not restart them: {{XONE}} with {{SHPE}} {{EXPD}} on {{CUT}} is a filter that opens on every hit of another track's drums and closes again.")),
                ("<code>DEST</code>", "What it moves; see below."),
                ("<code>FADE</code>", "Fade-in time after the note starts."),
                ("<code>PHAS</code> <code>DLY</code>", "Synth only: start phase, and a wait before the LFO begins."),
                ("<code>WHL</code> <code>VEL</code> <code>PRES</code>", fmt("Synth only: the mod strip, velocity, and the note's own pad pressure add to the depth. {{DPTH}} 0 with {{WHL}} up gives vibrato from the strip alone.")),
                ("<code>KTRK</code>", "Synth only: higher notes speed the LFO up.")])
            + "<h3>Destinations</h3>"
            + fmt("""
<p>An LFO can reach nearly everything its engine has a knob for. {{DEST}} starts with the engine's basic
destinations (pitch, level, pan, cutoff, resonance and the engine's main controls) and goes on with every other
continuous cell of the engine's pages, page by page; on the engines with two LFOs the list has the other LFO's
rate and depth ({{LRAT}}, {{LDPT}}). The {{STRIP}} and {{XIN}} pages, the slots and the pad-pressure routes offer
the same list. Plain turns go one entry at a time; [[Func]] + turn jumps to the start of
the next page's group, so the far end of a long list is a few clicks away.</p>
<p>A destination that is a page cell has that cell's name, or the name with a number or an F in front where two
pages use the same word ({{ATK1}} is operator 1's attack, {{FATK}} the filter envelope's). The LFO moves the cell
around the value its knob is set to, in the cell's own scale: at {{DPTH}} 100 a 0–100 cell swings by its whole range
and a time or frequency cell by half its range in octaves, never past the cell's limits. Turn the knob and the swing
moves with it. Every note has its own LFOs, so in a chord each note's cell moves by itself.</p>
<p>Two destinations stretch time rather than move a value: {{ETIM}} makes the amp envelope (and the envelope chain;
on FM every operator's) up to eight times as long or an eighth as long at full depth, {{FTIM}} the filter envelope,
without touching {{ATK}}, {{DEC}} or {{REL}}; moved under a held note, the running stage goes on faster or slower
from where it is. {{PM}} (wavetable, wave sequencer, drums, and the synth's strip, input and slots) and {{PM1}} to
{{PMAL}} (FM) are the oscillator's phase: from an LFO a vibrato of their own kind, from the track input or an
operator phase modulation.</p>""")
            + table(["Engine", "DEST offers (after OFF)"], [("Synth, the LFOs of MOD", dest_cell(SYNTH_DESTS.split())),
                                                              ("Synth: STRIP, XIN and the slots add", dest_cell(synth_more))] + dests)
            + fmt("""
<p>On the synth, {{1A}} to {{3B}} are the two {{SRC}} cells of each oscillator slot; {{ATK2}} to {{REL5}} and
{{EDPT}} are the extra envelopes of the {{MENV}} page. On the wave sequencer, {{P1}}–{{P16}}, {{L1}}–{{L16}} and
{{T1}}–{{T16}} are the pitch, level and time of one step. On the macro oscillator and the wave sequencer {{BMIX}}
to {{BWIR}} are the sixteen cells of {{BODY}} and {{BDY2}}. On the drum kit {{SWEP}} to {{ACRV}} are the kick
page's cells and {{MRPH}} to {{HARD}} the resonator drum's, each drum moved from its own value; {{WPOS}} is where a
drum's oscillator reads its table.</p>
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
        ("Mod slots", fmt("""
<p>The {{SLT1}} page and those after it hold sixteen mod slots, two to a page: {{SRC DEST AMT VIA}} |
{{SRC DEST AMT VIA}}. A slot joins one source to one destination: {{SRC}} is what it reads, {{DEST}} what it moves
(the list the LFOs offer), {{AMT}} how far, {{VIA}} a second source that scales the amount. A new page appears as
soon as a slot on the last one is used, up to {{SLT8}}. Every sound engine has them, with the sources below. On a
drum kit the sixteen slots are the kit's (every drum runs them with its own values), and each pad has two more of
its own ({{PSLT}}, Chapter 5).</p>
<p><b>How far.</b> {{AMT}} +100 with the source at its top is where an LFO at full depth peaks, in the
destination's own scale: pitch an octave (the amount counts squared: 10 is 12 cents, 50 three semitones), cutoff
four octaves, level from nothing to four times, pan the whole width, a 0–100 cell its whole range, a time or
frequency cell half its range in octaves. Negative moves the other way. Everything aimed at one destination adds
up: slots, LFOs, the strip's routes, pressure.</p>
<p><b>VIA</b> takes its source as 0 to 100 % (one that swings both ways counts from its bottom) and multiplies the
amount by it: {{VIA}} {{PRES}} gives the slot only while the pad is pressed, {{VIA}} {{STRP}} lets the strip bring
it in, {{VIA}} {{VEL}} makes it follow how hard the note was played. {{OFF}} leaves the amount as set.</p>
<p><b>Modulation of modulation.</b> A slot's {{DEST}} list goes on past the LFOs' list: {{L1RT}} {{L1DP}} {{L2RT}}
{{L2DP}} (on the synth to {{L4DP}}) are an LFO's rate (two octaves either way) and depth (up to double), {{AMT1}}
to {{AM16}} another slot's amount. {{ENV}} → {{AMT3}} makes slot 3 grow and fade with the note.</p>
<p><b>The track's effects.</b> Every {{DEST}} list (an LFO's, the strip's, the track input's, a slot's) ends with
the cells of the effects on the track: for each of the eight insert slots its eight cells ({{1:C1}} … {{1:C4}} are
the {{FX}} page, {{1:C5}} … {{1:C8}} the {{FX2}} page) and its {{WET}} ({{1:WT}}). After them the lists keep eight
reserved places ({{CH.R}} … {{RO.D}}) that move nothing: they were the cells of an effect block that is now four
insert types. [[Func]] + turn goes slot by slot. The list names an insert's cells by their place; the cell on the
display names them as the effect now in the slot does ({{1TIME}}, {{2:MIX}}, {{1:WET}}), or shows {{1:--}} where
there is nothing to move: an empty slot, a list or a count, and DRV, WSH, RED and AMP, which take no modulation.
At full amount the cell moves by its whole range from where its knob stands (a time or a frequency by ratio, never
past its limits); the effect glides there and the knob still reads as set. An effect belongs to the track, not to
a note, so one note speaks for all: the note played last, and when it has ended the one still sounding. With
nothing sounding the last one goes on (its LFOs run, its envelopes rest), so a free LFO sweeps a delay time in
silence. On the drum kit it is the drum struck last. The sends and the master section are not
destinations. A pad-pressure route of {{AT ROUTE}} reaches an effect's cell too: the pad of the note that speaks
for all moves it, and it goes back when that pad is let up.</p>
<p><b>How often.</b> Most destinations are moved every 16 samples at 48 kHz. Pitch, level, pan, cutoff, phase and
table positions are moved sample by sample (and FM's {{INDX}} and {{FDBK}}, the wavetable's {{BAL}}, {{FFM}} and
{{OFSB}}), which is what lets the track input or an oscillator act on them as FM,
AM or ring modulation. Nothing jumps: a slot changed or taken away under a sounding note is carried off over
4 ms.</p>
<p>The {{LFO1}}, {{LFO2}}, {{STRIP}} and {{XIN}} pages and the pressure routes of {{AT ROUTE}} are slots too, whose
source is fixed.</p>""")),
        ("Sources", fmt("""
<p>What {{SRC}} and {{VIA}} offer, in order ([[Func]] + turn jumps from group to group), the same on every sound
engine. The synth has two more LFOs and its extra envelopes in the list ({{LFO3}} {{LFO4}} {{ENV2}} … {{ENV5}}, after
{{ONE}}); five engines add sources of their own at the end.</p>""")
            + table(["Source", "What it gives"], [
                ("<code>OFF</code>", "Nothing: the slot is off."),
                ("<code>LFO1</code> <code>LFO2</code>", "The LFO's shape with its fade-in; the slot's AMT is the depth. (On a drum kit: the kit's LFOs as this drum runs them.)"),
                ("<code>ENV</code> <code>FENV</code>", "The amp envelope and the filter envelope, 0 to 100."),
                ("<code>VEL</code>", "How hard the note was played."),
                ("<code>PRES</code>", "This note's own pad pressure."),
                ("<code>STRP</code>", "The mod strip; the same for every voice."),
                ("<code>KEY</code>", "The note: 0 at note 60, ±100 five octaves up or down."),
                ("<code>RAND</code>", "One value a note, −100 to +100."),
                ("<code>IN</code>", "The level of the track input; the same for every voice."),
                ("<code>XIN</code> <code>XLP</code> <code>XHP</code> <code>XBP</code>", fmt("The track input itself, sample by sample: as it is, through the {{XIN}} page's low-pass ({{LPF}}), its high-pass ({{HPF}}), or both.")),
                ("<code>VIB</code>", fmt("The strip's vibrato wave (rate {{VRAT}} on {{STRIP}}), the same in every voice.")),
                ("<code>ONE</code>", fmt("A constant: an offset ({{ONE}} → {{CUT}} at −30 closes the filter a little), or something for a {{VIA}} to open and close.")),
                ("<code>RND2</code> <code>RND3</code> <code>RND4</code>", fmt("Three more values a note, independent of {{RAND}} and of one another: four random cells per note.")),
                ("<code>ONST</code>", fmt("A sound beginning in the track input (out of silence, or a jump of 4 dB above where it was): 100 at the hit, down to about a third 0.1 s later. The same for every voice. For a shape and a length of your own, an LFO with {{TRIG}} {{XONE}}.")),
                ("<code>BND1</code>–<code>BN16</code>", "The level of the track input in sixteen bands from 100 Hz to 8 kHz, five semitones apiece (the vocoder's bands), rising in 2 ms and falling in 60 ms: another track's spectrum as sixteen controls."),
                ("(the engine's own)", "Last in the list; see the next table."),
            ], "keys")
            + fmt("""
<p>{{RAND}} and {{RND2}} to {{RND4}} come from one generator per voice, which every other per-note draw of the engine
uses too (the wavetable's random scan, the sampler's grains): the same notes in the same order on a newly loaded
sound give the same values. On the wavetable, {{SEED}} on the {{ANLG}} page chooses the sequence.</p>""")
            + table(["Engine", "Its own sources", "What they give"], [
                ("FM", own_sources(params, "FM"), fmt("Each operator's output under its envelope and level, sample by sample, one sub-block (16 samples at 48 kHz) late. On {{PM1}} to {{PMAL}} a connection between operators that no algorithm has.")),
                ("Macro", own_sources(params, "Macro"), "The model's output before the filter, sample by sample, one sub-block late."),
                ("Wavetable", own_sources(params, "Wavetable"), fmt("Where the timed scan is (0 at {{POS}}, 100 at the table's end; 0 while the scan is off); the track input rectified, after the filters of {{XFLT}}; the track input through steeper filters at {{HPF}} and {{LPF}}. The two input sources arrive with no delay.")),
                ("Wave seq", own_sources(params, "Wave seq"), "The step each lane stands at: pitch (±24 semitones as ±100), shape, level, step time."),
                ("Sampler", own_sources(params, "Sampler"), "Where the playhead is between start and end; where the last grain started; the sample's own level at the playhead."),
            ], "cols3")),
        ("The track input", fmt("""
<p>Any track can listen to another: on {{TRK FX}}, page {{IN}}, {{SRC}} chooses the track. Its sound is then the
track input, the mean of its two sides, and the modulation can use it four ways: its level ({{IN}}), its onsets
({{ONST}}, and the LFO triggers {{XKEY}} to {{XHLD}}), its spectrum ({{BND1}} to {{BN16}}), and the sound itself,
sample by sample.</p>
<p>For the last, every sound engine has the {{XIN}} page: two routes, {{DST1}}
{{AMT1}} {{FLT1}} and {{DST2}} {{AMT2}} {{FLT2}}, and the corners of the two filters, {{LPF}} and {{HPF}}. {{FLT1}} /
{{FLT2}} choose what the route hears: {{RAW}} the input as it is, {{LP}} through the low-pass, {{HP}} through the high-pass,
{{BP}} through both. What it does depends on where it goes: on {{PTCH}} it is frequency modulation (an octave
either way at full), on {{CUT}} filter FM, on {{AMP}} amplitude modulation, and with a slot {{ONE}} → {{AMP}} at −100
beside it ring modulation, the plain sound gone; on a phase ({{PM}}, FM's {{PM1}} to {{PMAL}}) phase modulation, the
other track's sound as sidebands round every harmonic. A free slot with the source {{XIN}}, {{XLP}}, {{XHP}} or
{{XBP}} does the same with a {{VIA}} of its own. On the synth three destinations are made for the input: {{PM}}
(oscillator 1's phase), {{RING}} (the mixed oscillators times the input: at an amount of 100 the ring alone) and
{{SYNC}} (oscillator 1 restarts each time the input rises through a threshold; the amount is the sensitivity). On
the drum kit {{PM}} is the phase of each drum's oscillator. The resonator can also take the input into its body
({{IN}} on its {{EXC}} page).</p>
<div class="try"><b>Try it</b>Put a drum loop on track 1 and a held chord on an FM track. On the FM track: {{TRK FX}}
→ {{IN}} → {{SRC}} T1, {{MODE}} {{OFF}} (the drums are listened to, not mixed into this track); then on {{SLT1}} a slot {{ONST}} →
{{INDX}} at +60. The chord now flares brighter on every drum hit, though nothing in its own pattern changed. Give
{{LFO1}} a {{DEST}}, a {{DPTH}} and {{TRIG}} {{XKEY}}, and its cycle starts again on every hit too.</div>""")),
        ("Pad pressure", fmt("""
<p>Each pad sends its own pressure, and it reaches only the note that pad is playing. There are three ways to use it.</p>
<h3>1. Routes, per track</h3>
<p>{{AT ROUTE}} gives each track up to four routes. A route is a slot of the track's modulation router whose
source is the pad: a destination ({{DEST}}), a depth ({{DPTH}}) on the first page and, on the second, {{BIP}}
(the pad read both ways). Pressure counts for the note its pad is playing and for no other.</p>
<p><b>A preset decides where pressure goes.</b> The routes are cells of the track's sound, like its filter: {{SV}}
saves them with a preset, a project and a multi keep them, an undo step puts them back. An engine is made with its
own: pressure on the level ({{AMP}}) on the synth, FM, the macro oscillator, the wavetable and the wave sequencer;
on the decay ({{DEC}}) on a drum kit; on the exciter ({{EXC}}: a bow, a breath or a scrape as hard as the pad is
pressed) on the resonator; none on a sampler. A preset that stores no routes (the built-in ones but those that
show pressure, and presets you saved before 22 November 2026) is heard with its engine's own, whatever the track
had before; leave an audition without {{LD}} and the track's own routes are back with the rest of its sound.</p>
<p>{{DEST}} offers, in this order ([[Func]] + turn goes from group to group): {{OFF}}; what only pressure reaches
on the engine (the table below; {{VIB}} is the last of them); everything an LFO of the engine can move
(Appendix C), in the LFOs' order, the level as {{AMP}}; and last the cells of the effects on the track, shown by
the name the effect in that slot gives the cell ({{1:MIX}}, {{2TIME}}).</p>""")
            + table(["Engine", "Only pressure reaches", "What the route does there"], [
                ("Every engine", "VIB", "a vibrato that the pad brings in: at full pressure ± DPTH semitones (its rate is VRAT on the STRIP page)"),
                ("Resonator", "EXC", "the exciter's energy follows the pad (LAG smooths it); DPTH 100 = the pad is the whole energy"),
                ("Macro", "BOW", "the bow's force on the bowed model"),
                ("Drums", "—", "one route for the whole kit. DEC, PTCH, TONE, AMP, DRIV, SNAP and PAN are each drum's own response to its pad: depth 0 to 100, and BIP says KIT ONE WAY. Anything else is a route like a synth's"),
                ("MIDI track", "—", "no routes: per-note pressure is sent out")])
            + fmt("""
<p>A route gives pressure × depth in the destination's own scale: on the level a gain of 1 + pressure × depth, on
{{CUT}} four octaves at full depth, on {{PTCH}} an octave, on any other cell its whole range; a negative depth
lowers. A route to a cell of an effect moves that effect for the whole track, by the pad of the note played last.
The synth plays {{AMP}}, {{CUT}}, {{PW}}, {{MIX}}, {{XMOD}} and {{VIB}} on its own path, as it always has (its
level goes up to twice, pressure and an LFO together).</p>
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
factory preset, and nothing at all when it is at the bottom. What it does is yours to change. On every sound
engine the {{STRIP}} page (just before {{LFO1}} or, on the synth, before the screen's {{MOD}} page) has three routes,
{{DST1}} {{AMT1}}, {{DST2}} {{AMT2}}, {{DST3}} {{AMT3}}: each sends the strip to one destination of the list the LFOs
use (Func + turn jumps from group to group) by a signed amount in that destination's own scale, so +100 with the
strip fully up is where an LFO at full depth would peak. Pitch goes by the amount squared, up to an octave. Routes to
the same destination add up, {{LRAT}} and {{LDPT}} from the strip act on both LFOs, and an amount of 0 or a destination
of {{OFF}} switches a route off. {{WVIB}} is vibrato the strip brings in (semitones at the top of the strip) and
{{VRAT}} its speed. The amounts can be locked per step. Until you change the page, the strip does this:</p>""")
            + table(["Engine", "The mod strip by default"], [
                ("Synth", fmt("While an LFO in use has a {{WHL}} amount ({{MOD2}} page), the strip goes to those LFOs, adding to their depth; an LFO set to {{EXPU}}, {{ONE}}, 40 Hz, {{DPTH}} 0 passes the strip straight on to its {{DEST}} (that is how the factory presets open a filter or raise cross-mod from the strip). While no LFO in use has {{WHL}}, the strip follows the {{STRIP}} page's routes instead. The three {{ARCH}} sounds seed {{DST1}} {{CUT}} +50 and {{WVIB}} 0.25. While neither path gives the strip anything to do, it is vibrato, up to half a semitone.")),
                ("FM", fmt("{{DST1}} {{INDX}} +100: raises the modulation index, up to three times (brighter, harder; from the strip {{INDX}} counts double). {{WVIB}} 0.25: a little vibrato. It also moves the cutoff by {{WHL}} on the {{F ENV}} page, as on the next three engines.")),
                ("Macro", fmt("{{DST1}} the fourth macro ({{MORP}} in most models) +100: sweeps it to its top. {{DST2}} the first macro +30. {{WVIB}} 0.25. Cutoff by {{WHL}}.")),
                ("Wavetable", fmt("{{DST1}} {{POS}} +60: scans the table forward from {{POS}}, up to 60 % of the table. {{WVIB}} 0.25. Cutoff by {{WHL}}.")),
                ("Wave sequencer", fmt("{{DST1}} {{RATE}} +50: runs the sequence faster, twice as fast at the top (so the top of the strip is in time again). Cutoff by {{WHL}}.")),
                ("Sampler", fmt("Wears the sound down: {{DST1}} {{DRIV}} +70, {{DST2}} {{RATE}} +30 (sample-rate reduction, {{TONE}} page), {{DST3}} {{SPRY}} +50 (scatters the grains in {{GRAN}} mode). Whatever the strip adds to {{DRIV}} is mostly taken out of the level again, so the tone changes more than the loudness.")),
                ("Drums", fmt("Opens the whole kit up: {{DST1}} {{DEC}} +79, every drum rings up to three times as long (from its next hit), and {{DST2}} {{DRIV}} +50, driven harder. The routes belong to the kit: every pad follows them.")),
                ("Resonator", fmt("Plays harder and lets it ring: {{DST1}} {{ENRG}} +100, the exciter driven up to twice as hard, {{DST2}} {{DAMP}} −10, the high partials a little longer. {{WVIB}} 0.25.")),
                ("MIDI", "Sends controller 1 to the instrument outside."),
            ])
            + fmt("""
<p>The strip moves the selected track only, and every track keeps the value the strip last gave it. Set one track's
strip, select another and move the strip there: the first stays as you left it. Selecting a track changes nothing
until you touch the strip, and a preset you load on a track is given that track's value again. While a track's arp is on, the strip can drive one arp macro instead ({{STRP}} on the
arp's {{VARY}} page); the track's sound then keeps whatever the strip last gave it.</p>""")),
        ("Older sounds and projects (format 3)", fmt("""
<p><b>Format 3.</b> Since 22 November 2026 a project, a preset and a multi preset store every choice from a list
that can grow under its name as well as its place in the list: the {{DEST}} of an LFO page, of the {{STRIP}} and {{XIN}} pages
and of a slot, a slot's {{SRC}} and {{VIA}}, and an LFO's {{SHPE}}, {{TRIG}} and {{DIV}}. A file saved from now on
keeps those routings when a later version adds destinations or sources in the middle of a list. A file from an
older version holds places only and is loaded as it always was, place for place: where a list has grown since it
was saved, a routing can point at a neighbour, and the list below says where to look. Nothing in the file changes
until you save; saving writes format 3 with the names the cells then show, so look first. (A name a later version
does not know falls back to the stored place. Not yet stored by name: the {{DEST}} of the synth's {{MOD}} and
{{MENV}} pages and of a drum pad's {{PLFO}} and {{PSLT}}, whose lists only grow at their ends, and effect and
filter types, which are numbers that never move.)</p>
<p>On 21 and 22 November 2026 every sound engine moved onto one modulation router, the three effect blocks of
{{TRK FX}} became insert types, and several drum types became sounds of two machines. The factory presets of FM,
wavetable, wave sequencer and sampler sound as they did: FM's and the wave sequencer's sample for sample, the sampler's but one
(<i>Abyss Drone</i> plays another cloud of the same grains), the wavetable's within a hair (the sub oscillator,
the vibrato of <i>Drawbar Organ</i>, the two LFOs of <i>I You We Yeah</i>). A sound of your own may differ in these
ways:</p>
<ul>
<li><b>A slot aimed at an LFO or another slot.</b> In a slot's {{DEST}} list the new destinations ({{ETIM}},
{{FTIM}}, the phases, the body's cells) come before {{L1RT}} … {{AM16}}, so a saved slot aimed at one of those now
points further up the list: on FM nine places (a slot saved on {{L1RT}} now reads {{ETIM}}, one on {{AM16}} reads
{{AMT7}}), on the wavetable three, on the sampler one, on the macro oscillator ten and on the wave sequencer eleven
(two and three before {{BDY2}} came on 22 November, eight more since). Look at the slot's {{DEST}} and set it
again. On the macro oscillator and the wave sequencer the same eight places apply to anything (an LFO, the strip,
the input, a slot) that was aimed at a cell of the track's effects ({{1:C1}} …), and on the wave sequencer an LFO,
strip or input route saved on {{ETIM}}, {{FTIM}} or {{PM}} now reads {{BCNT}}, {{BGRN}} or {{BRED}}. Every other
{{DEST}}, and every {{SRC}}, kept its meaning.</li>
<li><b>Pad pressure and an LFO on the level.</b> Before, the pad's level and the LFO's tremolo multiplied, so the
tremolo deepened as you pressed; now everything aimed at {{AMP}} adds, and the tremolo swings by the same amount
however hard you press. To have the old sound, add a slot: {{SRC}} {{LFO1}}, {{DEST}} {{AMP}}, {{AMT}} the LFO's
{{DPTH}} times the route's depth, {{VIA}} {{PRES}}.</li>
<li><b>Pad pressure routes.</b> A project or a preset from before 22 November 2026 stores its routes in the old
words (level, cutoff, pitch, vibrato, pan, the engine's main handle); each is loaded onto the destination of that
meaning in the engine's list, a vibrato route as {{VIB}}, and does what it did. A project saved on 21 or 22
November with a route on anything those words cannot name lost that route at that save. And since a preset now
decides where pressure goes, loading a preset no longer keeps the routes the track had: set them again, or save
the preset with them.</li>
<li><b>DIRT, TALK and MODFX.</b> A project or preset saved with the three effect blocks of {{TRK FX}} loads them as
insert effects at the front of the track's chain, ahead of the inserts it had (the blocks ran first): {{DIRT}} and
{{TALK}} as the insert types of those names, and each effect of MODFX that was in use (chorus, flanger, phaser,
rotary speaker) as an insert of its own, so up to four slots where there was one page. Step locks on {{DIRT}} and
{{TALK}} cells move with them; step locks on MODFX cells are dropped; an LFO or a slot that was aimed at
{{CH.R}} … {{RO.D}} moves nothing now. Aim it at the cell of the insert instead ({{1:C1}} …).</li>
<li><b>Chorus, ensemble, vibrato.</b> {{CHR}} and {{ENS}} had a feedback cell where {{THRU}} is now: a stored
feedback of half or more loads as {{THRU}} on. {{VIB}} had {{KEY}} there: a stored {{KEY}} other than SELF loads as
{{THRU}} on, and the vibrato's trigger listens to its own track only.</li>
<li><b>Kicks.</b> A pad saved as {{FMKK}}, {{KICK}} or {{MKIK}} is the kick machine at that kick now: on loading,
its {{KICK}} and {{KCK2}} pages are worked out from the pad's own cells. It was matched to the old sound by
measurement; it is not the same sample for sample.</li>
<li><b>Toms, snares, rim, clave, hats, the cymbal machine.</b> A pad saved with one of the types that are no longer
in the {{TYPE}} list plays the nearest sound of the resonator drum (tom, the two snares, clave, rim, the cymbals)
or of the oscillator bank (the two hats). Close, not identical: level within 1 dB and brightness within a tenth for
the two factory kits that used the cymbal machine. The {{CYM}} page is gone.</li>
<li><b>The synth's track input.</b> A stored {{XIN}} / {{XDPT}} (the old cells) becomes the first route of the
{{XIN}} page: {{PM}}, {{RING}} or {{SYNC}} at that depth.</li>
<li><b>Two factory presets.</b> <i>Noise Sweep</i> and <i>Wind</i> are wavetable sounds on the {{NOIS}} table now;
they were synth sounds. A track that holds the old sound keeps it; the list offers the new ones.</li>
<li><b>PERF.</b> A project with one bank of pads loads it as bank 1 of four.</li>
<li><b>Randomness.</b> {{RAND}}, the wavetable's random scan and the sampler's grains, random start and random
slices are drawn from the one per-note generator: other values than before, with the same spread, and now the
same again for the same notes. The wave sequencer's {{LOOP}} {{RAND}} is a new, even walk; {{ORND}} is the old
one.</li>
<li><b>The old input cells.</b> FM's {{XIN}} and {{XDPT}} load onto the {{XIN}} page ({{DST1}} = {{PM1}} …
{{PMAL}}, {{AMT1}} = the depth); a step lock on them still acts but cannot be shown. The wavetable's old
{{XIN}} page cells are not loaded: set the input again (Chapter 5, Wavetable).</li>
<li><b>The wave sequencer's {{BODY}}</b>, if saved on 21 November 2026 (the page came that day and its cells moved
that night): its cells now sit at other numbers, so the old values land on the {{XIN}} page instead. Clear the {{XIN}} page and set {{BODY}} again.</li>
<li><b>Small changes.</b> With {{CHAN}} on, the wave sequencer's {{ENV}} source follows the envelope chain (it
gave nothing before). On the sampler a slot on {{CUT}} is heard even with the cutoff fully open. Cells a slot
moves on the wave sequencer (steps, envelopes, the body) move every 16 samples rather than every sample, and so does
the sampler's filter under an LFO on {{RESO}}, {{ENVF}} or {{FDRV}}.</li>
</ul>""")),
    ]))

    # =========================================================================================
    fx_rows = [(f"<code>{f['name']}</code>", ctx["fx_names"][f["name"]],
                " ".join(f"<code>{html.escape(s['label'])}</code>" for s in f["specs"] if s["id"] >= 0)) for f in params["effects"]]
    CURVE_ROWS = [(f"<code>{name}</code>", text) for name, text in [('SOFT', 'soft knee, then a slow rise instead of a ceiling'), ('HARD', 'hard clip'), ('FOLD', 'sine wavefolder: past the top the wave turns back on itself'), ('ASYM', 'two different knees: the upper half bends early, the lower late'), ('TANH', 'the true hyperbolic tangent: the textbook soft clip'), ('FAST', 'almost TANH, a little squarer at the top'), ('ATAN', 'arctangent: a rounder knee that never quite flattens'), ('ALGB', 'between TANH and ATAN'), ('SLOW', 'the gentlest knee: compresses long before it clips'), ('CUBE', 'cubic: clean until late, then a firm ceiling'), ('DIOD', 'an early, rounded knee, as of a diode pair'), ('TUBE', 'lopsided ceilings, as of a valve stage: even harmonics by itself'), ('TRI', 'triangle wavefolder: harder and brighter than FOLD'), ('RECT', 'full-wave rectifier: the octave above (thin when driven hard)'), ('HALF', 'half-wave rectifier: the lower half cut away'), ('XOVR', 'a dead zone round zero: quiet sound vanishes, loud sound rasps'), ('EVEN', 'adds the second harmonic, more as the level rises'), ('CHB3', 'a full-level sine comes out as its third harmonic; quiet sound passes three times louder')]]
    ch.append(("Effects", [
        ("Signal path", fmt("""
<p>Engine → the track's eight insert slots → mixer (volume, pan) → master EQ, compressor, limiter.
Each track also sends to two shared buses, each a chain of effects of its own. All of a track's effect pages, and
the buses', are on {{TRK FX}}.</p>
<p>Wherever an effect sits (a track's slot, a send bus, a {{PERF}} pad, the master) it is the same thing: one of
the 33 types of one list, with the same cells, mixed the same way: the untreated sound times (1 − {{WET}}) plus the
effect times {{WET}}; on a send bus the effect alone. The master is such a chain too, with a fixed content: its
three effects are the EQ, the compressor and the limiter of {{SETTINGS}} → {{MASTR}}, and nothing else can be put
on it from the controller.</p>
<p>{{DIRT}} (distortion, bit and rate reduction and a tuned feedback) and {{TALK}} (a vowel filter) are two of the
insert types; they were fixed blocks with pages of their own before 22 November 2026, together with a third, MODFX,
whose chorus, flanger, phaser and rotary speaker are the insert types {{CHR}}, {{FLG}}, {{PHS}} and {{ROT}}
(Chapter 7, <i>Older sounds and projects</i>, says how a saved project loads).</p>""")),
        ("Insert effects", fmt("""
<p>The {{FX}} page shows one insert slot: {{SLOT}} chooses which, {{TYPE}} the effect, {{WET}} how much of it, and
knobs 5 to 8 are the effect's own four controls.</p>
<div class="note"><b>Careful</b>Turning {{TYPE}} replaces the effect and its four values at once, without the four-click
confirmation. Undo ([[Func]] + [[Shift]] + [[Play]]) brings the effect back as it was; a scroll through the list
counts as one step.</div>
<p>An effect with more than four controls has the rest on the next page, {{FX2}}, under knobs 5 to 8: the same slot,
shown again by {{SLOT}} and {{TYPE}}. They are saved, locked per step and given pressure ranges like the others.</p>""")
         + table(["TYPE", "Effect", "Knobs 5–8, then page FX2"], fx_rows, "cols3")
         + fmt("""
<h3>Through zero</h3>
<p>{{FLG}}, {{CHR}}, {{ENS}} and {{VIB}} have {{THRU}} on page {{FX2}}. On, the dry sound is delayed by the effect's
{{TIME}}, so the moving delay passes through the dry sound instead of stopping short of it, and at the crossing the
two cancel completely: the hollow sweep of two tape machines. The effect is then {{TIME}} late.</p>
<h3>The reverbs</h3>
<p>{{RVB}} (a plate), {{HALL}}, {{SPRG}} (a spring) and {{SHIM}} share their cells: {{DCAY}}, {{DAMP}} and {{PDLY}}
on all four, and {{SHIM}} with {{MODE}} on all four: the tail fed back into itself shifted by {{MODE}}'s interval
(an octave up, a fifth up, both, an octave down, backwards, two octaves down), so any of them can shimmer. {{MOD}}
(hall, spring, shimmer) makes the tail waver; {{DISP}} (spring, shimmer) is the spring's chirp. {{HOLD}} on {{RVB}}
and {{HALL}}, or {{MODE}} at <code>HOLD</code> on any of them, shuts the way in and lets what is in the reverb go
round without loss until you let it go. The spring, the shimmer and the freeze of the {{PERF}} pads are settings
of one reverb network.</p>
<h3>Effects that listen: KEY</h3>
<p>{{CMP}}, {{TRNS}}, {{FLT}}, {{PHS}}, {{TRM}}, {{PAN}} and {{WAH}} have a {{KEY}} cell ({{DUCK}} calls it
{{SRC}}): what the effect listens to. <code>SELF</code> is the sound it works on, <code>T1</code> to
<code>T16</code> another track, <code>IN</code> the track's input ({{TRK FX}} → {{IN}}). A compressor keyed from
the kick ducks; {{ENV}} on {{FLT}} and {{PHS}} moves the sweep with the keyed sound's level; {{TRIG}} on {{TRM}},
{{PAN}} and {{WAH}} (and {{VIB}}, from its own track) restarts the effect's LFO on a hit. It works on a send bus
too, and a track that is listened to goes on feeding its listeners while it is muted.</p>
<h3>The vocoder</h3>
<p>{{VOC}} shapes its track with the track's input (set {{SRC}} on {{TRK FX}} → {{IN}}; {{MODE}} there can stay
<code>OFF</code>). {{MODE}} <code>BAND</code> is the classic vocoder: the track's sound through sixteen bands whose
levels follow the input's, {{BAND}} shifting one set against the other, {{NOIS}} adding hiss for the consonants,
{{REL}} how fast a band falls. <code>TUNE</code> drives a tuned body ({{BODY}}: the resonator's line of bodies) at
the pitch of the track's last note with the input's spectrum, ringing on for {{RING}}. {{FRZ}} holds the bands
where they are.</p>""")),
        ("Saturation curves", fmt("""
<p>One list of curves is shared by everything that lets you choose how a sound is bent: the waveshaper insert
({{WSH}}, its {{CURV}}), the {{DIRT}} insert's {{TYPE}}, the amp insert ({{AMP}}, {{SAT}} on page {{FX2}}: the
curve of both its stages) and the dub echo on {{PERF}} ({{SAT}} on its {{MORE}} page: the curve of its tape).
The amp starts on <code>TANH</code>, the dub echo on <code>FAST</code>.</p>
<p>On the waveshaper, {{ASYM}} works the chosen curve off-centre, which adds even harmonics (the octave) to any
of them; at 0 a curve that treats both halves of the wave alike gives odd harmonics only.</p>""")
         + table(["Curve", "What it does"], CURVE_ROWS, "cols2")
         + fmt("""<div class="note"><b>Good to know</b>The folders, the rectifiers and <code>HARD</code> make very bright
harmonics; on high notes with the drive up some of them fold back as rough, unrelated tones. The amp is the
cleanest place to use them: it works at twice the sample rate.</div>""")),
        ("Tempo sync", fmt("""
<p>An insert that has a {{TIME}} (the delays) or a {{RATE}} (chorus, flanger, phaser, tremolo, auto-pan, ensemble,
ring modulator, vibrato, wah) shows {{SYNC}} in cell 3 in place of {{ON}}. With {{SYNC}} on, that cell picks one of
22 divisions from 1/128 to 16 bars, triplets and dotted included, and follows the tempo. The free value is kept for
when you switch sync off. A division too long for the effect is halved until it fits. To take a synced effect out,
turn {{WET}} to 0.</p>""")),
        ("Sends and master", fmt("""
<p>{{SND1}} and {{SND2}} on each track feed two buses shared by all tracks: a reverb and a delay to begin with.
Each bus is a chain of up to eight effects of any insert type, one after the other, set on its own pages of
{{TRK FX}}: {{S1FX}} / {{S1FX2}} and {{S2FX}} / {{S2FX2}}, laid out as {{FX}} and {{FX2}} are ({{SLOT}} chooses the
effect, {{TYPE}} at {{OFF}} takes it off the bus). A bus returns only what its effects make: an effect put on a bus
starts with {{WET}} at 100, and a bus with no effect returns nothing. Turn {{WET}} down on an effect in the middle of
a chain to let some of what came before it pass. An effect with a {{KEY}} or {{SRC}} cell can listen to a track from
a bus too (a compressor after the reverb keyed from the kick: the reverb ducks). {{BUS1}} and {{BUS2}} on the
{{TRACK}} page are the type of the first effect on each bus. The buses are saved with the project.</p>
<p>The master EQ ({{LOW MID MFRQ HIGH}}), compressor ({{THR RATI MAKE}}) and limiter ({{CEIL}}) are on
{{SETTINGS}} → {{MASTR}}; master volume is {{MVOL}} on {{SETUP}}. The effects of the {{PERF}} pads aimed at
<code>MSTR</code> sit just before them.</p>
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
({{S5}}). After the step, the knob returns to its normal value. Hold several steps and the same turn locks all of them to
the value shown. Engine controls, mixer levels and sends, the track's insert effects, the mod slots' and the
strip routes' amounts, and the arp's gate, velocity and ratchet can all be locked. Cells that choose a sample or
rebuild a table, the track's input source and the send buses' effects cannot.</p>
<p>Remove one lock: hold the step (or several), hold [[&gt;]], turn the knob. Remove all of a step's locks: {{LCKS}} on its
{{TRIG}} page, anticlockwise (an undo step).</p>
<p><b>Pressure into locks.</b> On a step with recorded pad pressure, {{>LCK}} on its {{PRES}} page (four clicks)
writes the swell as locks: on every step the note spans, each cell the track's pressure routes point at is locked
to where the average pressure over that step would hold it. A route's depth is how far full pressure moves the cell
from its own value toward its top (toward its bottom when the depth is negative). Steps under the note that had no
trig become trigs that carry only locks, with no note of their own. Chords on a synth track share the cells, so
their pressures are averaged; on a drum track each pad's own cell is locked. A route whose destination is not a
cell of a page (the pitch, for one) is left out and the name bar says so
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
<p><b>Metronome.</b> While recording runs a click marks every beat, higher and louder on the first beat of each
bar. {{CLCK}} on {{SETTINGS}} → {{CLICK}} chooses when it sounds: <code>OFF</code>, <code>REC</code> (only while
recording runs: the setting it starts with) or <code>PLAY</code> (whenever the sequencer plays). {{CLVL}} sets its
level; each turn of that knob plays one click, so you can set it before you start. The click follows the tempo,
stops with the sequencer, and is only for your ears: it does not pass through the master effects or {{MVOL}}, and
it is never recorded when you resample a track or the master.</p>
<p><b>Count-in.</b> Set {{CNT}} on the same page to <code>1BAR</code> or <code>2BAR</code> and a recording starts
with that many bars of clicks: [[Rec]] + [[Play]] (or [[Play]] with recording already on) shows
<code>COUNT IN 1 BAR</code>, the metronome counts four beats to the bar — also when {{CLCK}} is off — and the
sequencer and the recording start together on the beat after. During the count-in nothing plays, nothing you play
is recorded, and a queued pattern waits. [[Play]] during the count-in calls it off. The count-in is only for a
recording that starts the sequencer: if the sequencer is already playing, recording switches on at once, and
[[Play]] without recording always starts at once.</p>
<p><b>Knob movements are recorded too.</b> On {{PARAM}}, {{TRK FX}} and {{FLTR}}, a knob turned while the
pattern records and plays is written into the selected track as parameter locks: one on every step that passes,
from the first turn for one lap of the track, so a filter sweep comes back exactly as you made it. A knob you stop
turning keeps writing its value until the lap is full. A step that had no trig becomes a trig without notes that
only carries the lock. Afterwards the track's own value is what it was before you touched the knob; hold a step and
[[&gt;]] + turn removes a lock, and undoing the recording pass removes the whole movement. Sample choice, slicing
and wavetable key frames cannot be locked and are not recorded.</p>
<p><b>While recording</b> the name bar shows <code>REC FREE</code> or <code>REC QUANT</code> on every screen (a
single <code>R</code> or <code>Q</code> when room is short) and a red light runs across the pads, one pad per step
of the selected track. Pads that play notes keep playing them under the light; on {{STEP}} the playhead is the
light, and recorded notes appear on the pads as you play them. Keys are written into the selected track, pad hits
(each repeat of a roll included) into the track the pads play, arp output into the arp's track, and the moves of
the {{PERF}} pads into the pattern as a take (Chapter 4). Notes played
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
the display shows that track's {{MAIN}} page for it; turn a knob to lock a parameter on that step. (A pad pin is let go when recording ends, so that the pads show the steps.)</p>""")),
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
<p>{{CLK}} on {{SETTINGS}} sets MIDI clock: {{INT}} (Neptune is the clock and sends it), {{EXT}} (follow incoming
clock) or {{AUTO}} (follow a clock while one comes in, be the clock when none does). {{LINK}} is listed for the
future and does nothing yet.</p>
<p>While Neptune follows, the other machine's Start and Stop start and stop the sequencer, its tempo is the tempo,
and the sequencer is kept on its beat (it is pulled back gently, by 3 % of the tempo at most, so after a large
tempo jump it can take some seconds to be exactly on the beat again). A start is always from the top of the
pattern. [[Play]] still starts and stops by hand. In {{AUTO}}, when the clock goes away without a Stop the music
carries on at the last tempo and Neptune sends clock again after a second.</p>""")),
        ("With Ableton Live or another DAW", fmt("""
<p><b>Tempo and transport from the DAW.</b> While the app runs, the Mac has a MIDI port called <i>Neptune Clock
In</i>. In Live: Settings → Link, Tempo &amp; MIDI → under MIDI Ports find the output <i>Neptune Clock In</i> and
switch <b>Sync</b> on. On the Launchkey set {{CLK}} to {{EXT}} or {{AUTO}}. Live's play and stop now start and stop
Neptune and its tempo leads. If Neptune is early or late against Live, adjust the port's <i>MIDI Clock Sync
Delay</i> in Live. Any other program that sends MIDI clock works the same way. The Launchkey's own ports are never
taken as a clock.</p>
<p><b>Neptune's sound in the DAW.</b> Neptune plays through the Mac's audio output. To record it on a track of the
DAW, send that output into the DAW: with an audio interface, by cable; inside the Mac, with a loopback audio device
(BlackHole and Loopback are two) chosen as Neptune's output and as the track's input. A plug-in build (AU and VST3)
exists as an experiment only: it plays the starting synth sound from the DAW's notes and has neither the
controller, the screens nor your project yet.</p>""")),
    ]))

    # =========================================================================================
    arp_modes = ["Up", "Down", "Up-down", "Random", "As played", "Chord repeat", "Walk", "Play order", "Converge",
                 "Diverge", "Poly arp (all notes, accented)", "Random with octave jumps",
                 "Parallel (PARA: every key runs its own climb through the octaves from the moment it is pressed, "
                 "side by side with the others; keys pressed together climb together)",
                 "Pedal (PEDL: the lowest note sounds before each higher one in turn — 1 2 1 3 1 4)",
                 "Rising pedal (PDL+: the same, and after each pass the next note up becomes the pedal — "
                 "1 2 1 3 1 4, 2 3 2 4, 3 4)"]
    ch.append(("Arpeggiator and rolls", [
        ("The arpeggiator", fmt("""
<p>Every track has its own. [[Arp]] opens the screen; pressed again there, it switches the selected track's arp on.
[[Shift]] + [[Arp]] latches it so it carries on after you let go (with the arp off, the same gesture latches the
keys themselves). Arps follow the transport, the scale lock and the tempo.</p>
<p><b>Transposing a latched arp:</b> latch a chord, let go, then press a pad that plays a single note (the
<code>SCALE</code>, <code>PRESET</code> and <code>AT ROUTE</code> pads). The arpeggio carries on without restarting,
moved so that its lowest note is the pad's note. Mutations and a frozen loop move with it. A key, or a chord pad,
still starts a new chord.</p>""")
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
<p>With {{ROLL}} on, any pad that plays a note ({{DRUMS}}, {{SCALE}}, {{CHORD}}, {{AT ROUTE}}, {{PRESET}}) repeats while held.
Pressure raises the rate from {{BASE}} to {{MAX}}: tempo divisions in {{QUANT}} mode, free Hz in {{FREE}} mode.
{{P>V}} lets pressure set the loudness of each repeat; {{DCAY}} fades them.</p>
<p>{{ROL2}} refines it: swing, a glide between rates, a pressure threshold so light touches play single hits
({{THRS}}), latch, humanize, an accent every Nth hit, a probability per repeat and a limit on the number of repeats.
Rolls are recorded hit by hit.</p>""")),
    ]))

    # =========================================================================================
    ch.append(("Sampling, slicing and stretch", [
        ("Samples", fmt("""
<p>The sampler's {{SMPL}} cell chooses from the bank: a built-in drum loop, eight hits and a tone, any WAV or AIFF
file in <i>Music / Neptune / Samples</i> (read at start-up), eight drum breaks (below), and your resampled takes.
Set start, end and loop on the
{{SAMPL}} page or by eye on {{WAVE}}; [[Func]] + turn is coarse and [[Shift]] + turn fine, and zooming ({{ZM}})
makes the markers finer still.</p>
<p>Play modes: {{FWD}}, {{REV}}, {{1SHT}} (plays to the end regardless of the key), {{GATE}} (stops on release),
{{LOOP}}, {{PING}} (loops back and forth), {{GRAN}}.</p>
<p>{{ROOT}} is the key at which the sample plays at its recorded pitch; {{NORM}} brings a quiet sample up to full
level.</p>""") + '<div class="shots">' + S.svg("WAVE", 1, "WAVE") + S.svg("PARAM sampler", 2, "Sampler, SAMPL") + "</div>"),
        ("Drum breaks", fmt("""
<p>Eight drum breaks come with the app, after your own files in the {{SMPL}} list. The name is the tempo the break
was played at: B080, B090, B105, B120, B157 and B160 are recordings of drummers, released by them into the public
domain (CC0; names and sources are in <i>docs/sample-credits.md</i>), and B100 and B170 are made by the app's own
drum voices. Each is one, two or four whole bars. Choosing a break sets {{LEN}} on the {{STRCH}} page to its length,
so turning {{SYNC}} on is all it takes to play it in time with the project.</p>
<p>Every break has two sampler presets on the {{PRESET}} screen (category PERC):</p>
<ul>
<li><b>Break 90 Loop</b>: hold a key and the break goes round in time with the tempo, whatever
tempo it was played at. It is cut at its drum hits ({{MODE}} = {{TRNS}}) and played with {{ALGO}} = {{SLCE}}, so
each hit sounds as recorded and only its place in time moves; other keys play it higher or lower without changing
the timing. One voice: a new key starts the break again.</li>
<li><b>Break 90 Slices</b>: the sixteen strongest hits, one on each key going up from C4
({{PLAY}} = {{KEYS}}, play mode {{1SHT}}), at their recorded speed and pitch. The pads of {{PRESET}} play sixteen keys in
a row from the C of the track's octave: at C4 that is one slice per pad. Record them into steps to build your own beat
from the break.</li>
</ul>
<p>From there everything on the sampler's pages applies: {{ALGO}} = {{TAPE}} for the repitched sound, {{PLAY}} =
{{RAND}} or {{NEXT}} on the slices, {{SLCS}} for fewer or more of them.</p>""")),
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
<p>A preset to learn granular playing from: <i>Abyss Drone</i> (PAD) reads the built-in cymbal two octaves down
in {{GRAN}} mode - long grains ({{SIZE}} 0.35 s), about ten at a time ({{DENS}} 30), the read position creeping
({{SCAN}} 0.05) through the cymbal's ring, every grain starting somewhere else ({{SPRY}}), half of them backwards
({{RVRS}}), spread between the speakers ({{SPRD}}), under a low-pass that LFO 2 lets drift. Turn each of those
cells towards its other end to hear what it contributes: short {{SIZE}} or low {{DENS}} breaks the drone into
drops, {{SCAN}} at 1 plays the cymbal's own decay, {{SPRY}} at 0 makes it buzz on one spot.</p>
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
selected track: as its sample on a sampler track, as its {{USER}} table on a wavetable track (or, while that
track's table is {{KEYS}}, as the sample of the key chosen on the {{WAVE}} page).</li>
</ol>
<p>{{NM}} normalises takes, {{SM}} picks a different sample for the track, and {{CR}} (four clicks) crops the sample
to its markers.</p>""") + '<div class="shots">' + S.svg("WAVE", 2, "WAVE, REC page") + "</div>"),
    ]))

    # =========================================================================================
    ch.append(("Projects, presets and the rest", [
        ("Projects", fmt("""
<p>A project is everything: tempo, all 16 patterns with their steps and locks, every track's engine and sound,
its insert effects, the two send buses' chains, the master section, the song, the scale pages, the chord pads, the
{{PERF}} pads (all four banks) and the takes recorded with them, pressure routes and ranges, arp settings, and what
each synth track's {{ARCH}} settings were left at. There are 32 slots
(<i>Music / Neptune / Projects</i>, P01 to P32).</p>
<p>{{SETTINGS}} → {{PROJ}}: {{SLOT}} chooses, {{LOAD}}, {{SAVE}}, {{SVAS}} (save as, to the chosen slot) and {{NEW}}
each need four clicks and answer OK or FAIL.</p>
<p><b>Naming a project:</b> {{SVAS}} saves at once, so nothing waits for the Mac. The Mac then puts the save box in
front of everything, "Save the project in P05", with the project's name selected so that typing replaces it (13
characters at most). Return or Save names the project. Escape or Cancel takes the save back: the slot keeps the
project it held, or stays empty, and your project is the one it was before. Left unanswered, the save stays as it
is. {{SAVE}} writes the current slot again without asking. The line under the cells shows the chosen slot's name:
"P05 Night Drive", with NOW before the name on the slot your project is in; a project nobody has named reads
"P05 USED".</p>
<p>The app reopens
the last project when it starts, and on top of it everything exactly as it was when the app was last running, saved
or not (the app notes its state when it quits and every 15 seconds). {{SAVE}} is still what writes a project slot;
{{NEW}} or {{LOAD}} is the way to a different starting point.</p>
<p><b>Projects and presets from an earlier version</b> load as they were saved. Wherever a cell chooses from a list
that has grown since (the destination of an LFO, of a strip route or of a mod slot, a slot's source, an LFO's shape,
a filter's type, a drum pad's type), a file now keeps the entry's name, and a file from before is read by the list
of the version that wrote it: a drum kit saved with version 0.9 comes back with every pad on the drum it had (a
snare, a tom or a hat of that time is now the resonator drum's or the oscillator bank's sound of that name, a
{{KICK}} the kick machine at that kick). If a file names something this version no longer has, that route loads
as OFF and the app's log says which one (<i>Library / Logs / NeptuneGroovebox.log</i>).</p>
<p>A project also holds the keyboard: its split and the octave chosen with [[Oct−]] / [[Oct+]]; both are set again
when it loads and when the app starts.</p>
<p>Not saved: the notes an arp has mutated to (whether mutate is running is saved), the temporary pattern copy and
the clipboards. Not in a project because they belong to the instrument: the metronome's and the help cards'
settings, and the chord sets of {{CHORD}} → {{SETS}}.</p>
<p>Files are written in format 3, which names what it routes; what a file from an older version does when it is
loaded is in Chapter 7, <i>Older sounds and projects</i>.</p>""")),
        ("Presets", fmt("""
<p>A preset is one track's sound. The built-in ones are marked * in the list, among them a loop and a sliced preset
for each of the sampler's drum breaks (chapter "Sampling, slicing and stretch"); yours are saved from {{PRESET}} with
{{SV}} into <i>Music / Neptune / Presets</i>. Categories ({{CT}}): INIT, BASS, PAD, LEAD, KEYS, FX, PERC, KIT, and
MULT for the multi presets (below). The resonator's drum sounds are under PERC with the other percussion.</p>
<p><b>What {{SV}} saves:</b> the track's own sound, complete - every engine setting with its modulation (LFOs,
slots, strip and input routes), the track's insert effects and their settings, the send levels, the volume, the
track's arpeggiator (all its
settings, whether it is on, its latch, whether mutate runs, and its tapped rhythm), and the keyboard: the split
and the octave. If a preset is being auditioned at
that moment, it is still your own sound that is saved, not the one you are listening to. A preset saved this way
brings its effects, its arpeggiator and its keyboard with it when it is auditioned or loaded. A preset always
replaces the track's effects: one that stores none (most built-in presets) leaves the track with no insert
effects, so nothing of the previous sound's effects is left; the track's volume, pan and sends stay unless the preset stores them. Leaving an audition without {{LD}}
puts the track's own effects back. The built-in presets have no arp or keyboard of their own and leave those
alone. Presets you saved before
these were stored (effects and arp: 14 November 2026; latch and keyboard: after it) do not have them: load the
preset, set things as you want them and save it again.</p>
<p>The keyboard is one for all tracks, so a preset that stores it moves the keys for every track. While you audition,
leaving puts your own split and octave back - unless you pressed [[Oct−]] / [[Oct+]] or changed the split yourself
in the meantime: what you set by hand stays. The app learns of an octave press within two seconds (the buttons
send nothing), so give it that long before saving.</p>
<p><b>The save box:</b> {{SV}} saves at once as User 1, User 2 and so on, so nothing waits for the Mac. The Mac
then puts a small window in front of everything, "Save the sound of track N as a preset", with a name in it,
selected, so that typing replaces it. The name is that of the preset the track's sound came from: the one you last
kept with {{LD}} or last saved under. It is remembered for each track, with the project and across restarts. A
sound that came from a built-in preset is offered under that name with a number (Deep Bass 2), since a built-in
preset cannot be saved over; a track whose sound came from no preset, or whose engine you have changed since, is
offered the User name. The line under the name says what Return will do: keep it as a new preset, or replace
your preset of that name. Return, or the Save button, does it; names have up to 15 characters. Escape, the
Cancel button or closing the window takes the save back: no preset, no file, and the list and the track are as
they were before {{SV}} (the name bar says NOT SAVED). A built-in preset's name cannot be typed in either: the
line says so and Save is off until the name is another one. If you save again before answering, the earlier save
stays under its User name and the window asks about the new one; so does a save whose window is still open when
you quit.</p>""")),
        ("Multi presets", fmt("""
<p>A multi preset is several tracks that belong together: a kit and the pad it ducks, a loop and the resonator it
plays through. It holds each member's sound, a starter pattern for each, and the connections between them (a
track's input, a {{DUCK}} insert's {{SRC}}, any effect's {{KEY}}), which are turned into the right track numbers
wherever the multi lands.</p>
<p><b>Loading:</b> on {{PRESET}} set {{CT}} to MULT. The name bar says where the highlighted multi would go
(<code>2TRK&gt;T3-4</code>: the earliest empty tracks) or <code>NEED 2 EMPTY</code>. {{LD}}, four clicks, loads every
member as one undo step. A multi never replaces a track that holds something: clear tracks or their sounds first
if there is no room.</p>
<p><b>Saving:</b> {{SV}} on a track that listens to another track (its input, a keyed effect) or that another
track listens to saves all the connected tracks as one multi; any
other track is saved as a single preset. Your multis are files beside your presets
(<i>name</i><code>.multi.json</code>).</p>""") + multi_table(params)),
        ("The built-in presets", fmt("""
<p>The list as {{PRESET}} has it, by category and engine. Set {{EN}} to ANY to see presets of other engines than
the track's; loading one changes the track's engine.</p>""") + preset_table(params) + fmt("""
<h3>Presets that show one thing</h3>
<p>Twenty-five of the built-in presets are each made to show one thing the instrument does. Find them by name
({{EN}} on ANY, {{CT}} as the table says), play what the line says and move the control it names.</p>""") + show_table(params)),
        ("Keyboard split", fmt("""
<p>[[Shift]] + [[Oct−]] drops the lowest twelve keys by one further octave, then two, then three, then back to
normal; [[Shift]] + [[Oct+]] steps the other way. The name bar shows {{SPLIT -1 OCT}}. Plain [[Oct−]] / [[Oct+]]
still shift the whole keyboard. Split and octave are saved with projects and with your presets (see "Presets").</p>""")),
        ("Latency and headroom", fmt("""
<p>Neptune adds about 1.3 ms of its own between a key and its sound. What you hear beyond that is the audio output:
an interface or wired headphones add a few milliseconds, the Mac's built-in speakers about 19 ms.</p>
<p>The heaviest session measured so far (eight tracks, eight notes each, distortion, vowel filter, chorus, delay
and reverb on every track; measured before the engines moved onto the one router) used about a quarter of one
processor core on an M3 Pro. You are unlikely to run out.</p>
<p>The display redraws about twelve times a second, which is as fast as the Launchkey accepts pictures.</p>""")),
        ("If something seems wrong", table(["What you see", "What to check"], [
            ("No sound", fmt("{{MVOL}} on {{SETTINGS}}; the track's {{VOL}}; whether it is muted (red pad on {{TRACKS}}) or another track is soloed (blue).")),
            ("Pads do not play the drums", fmt("Only {{DRUMS}} has drum pads: [[Shift]] + bottom pad 2. The keys play the selected drum track on every screen.")),
            ("A knob does not seem to change the sound", fmt("A step lock or a pressure range may be overriding it. Orange pads on {{PARAM}} have locks; a violet pad has a range.")),
            ("Pads play notes on a screen where they should not", fmt("They are pinned ({{PIN}} in the name bar). On {{DRUMS}}, {{CHORD}}, {{SCALE}} or {{PERF}}, hold [[Shift]] and hold that screen's bottom pad for half a second.")),
            ("Notes hang or the arp will not stop", fmt("Latch is on ({{LATCH}} in the name bar): [[Shift]] + [[Arp]].")),
            ("A sound or an effect rings on and will not stop", fmt("[[Shift]] + [[Play]] stops the sequencer, lets every note go and cuts every effect's tail.")),
            ("Low keys are in the wrong octave", fmt("A keyboard split is on: [[Shift]] + [[Oct+]] until the {{SPLIT}} message says off.")),
            ("Copy or paste did nothing", fmt("They act only on {{STEP}}, {{TRACKS}}, {{PARAM}}, {{FLTR}} and {{TRK FX}}, or on a held step. On {{TRACKS}} clear and paste need the button held for half a second.")),
            ("A synced delay is shorter than the division says", "The division was longer than the delay can hold and has been halved to fit."),
            ("A send returns nothing", fmt("Its bus has no effect on it: {{TRK FX}} → {{S1FX}} or {{S2FX}}, and give slot 1 a {{TYPE}}.")),
            ("Pressure does something else after loading a preset", fmt("The routes are part of a sound: a preset that stores none is heard with its engine's own (Chapter 7, Pad pressure). Set them on {{AT ROUTE}} and save the preset with {{SV}}.")),
            ("A routing of an old sound moves the wrong thing", fmt("Its list has grown since the sound was saved: Chapter 7, <i>Older sounds and projects</i>.")),
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
        ("Stop, rewind and silence everything", fmt("[[Shift]] + [[Play]]")),
        ("Tap tempo", fmt("[[Func]] + [[Settings]], tapped")),
        ("Arp on / off", fmt("[[Arp]] (opens the screen), [[Arp]] again")),
        ("Latch", fmt("[[Shift]] + [[Arp]]")),
        ("Scale lock / scale screen", fmt("[[Scale]] / [[Shift]] + [[Scale]]")),
        ("Step through the scale pages", fmt("Hold [[Scale]] + [[▲]] / [[▼]]")),
        ("Another bank of chord or effect pads", fmt("[[▲]] / [[▼]] on {{CHORD}} or {{PERF}}")),
        ("Store a chord on a pad", fmt("On {{CHORD}}: hold the keys, press the pad")),
        ("Pressure range on a knob", fmt("Turn it, then [[Func]] + [[▲]] (high end) or [[▼]] (low end); again clears")),
        ("Pin the playing pads", fmt("On {{DRUMS}}, {{CHORD}}, {{SCALE}} or {{PERF}}: [[Shift]] + hold that screen's bottom pad half a second")),
        ("Confirm something destructive", "Four clicks clockwise on its knob"),
        ("Split the keyboard", fmt("[[Shift]] + [[Oct−]] / [[Oct+]]"))], "keys")
    ch.append(("Gestures at a glance", [(None, cheat)]))

    # =========================================================================================
    divs = choices(params, "FM Synth", "LFO1", "DIV")
    fxdiv = ["1/128", "1/64", "1/32T", "1/32", "1/16T", "1/32D", "1/16", "1/8T", "1/16D", "1/8", "1/4T", "1/8D", "1/4",
             "1/2T", "1/4D", "1/2", "1/2D", "1BAR", "2BAR", "4BAR", "8BAR", "16BAR"]
    lists = table(["List", "Values, in order"], [
        ("Engines (<code>ENG</code>)", "Synth · FM · Macro oscillator · Wavetable · Wave sequencer · Sampler · Drums · MIDI track · Resonator"),
        ("Oscillator waves", " ".join(choices(params, "Unified Voice", "OSC", "WAV1"))),
        ("Filter types", " ".join(choices(params, "Unified Voice", "FILTR", "TYPE"))),
        ("LFO shapes (<code>SHPE</code>)", " ".join(lfo_shapes)),
        ("LFO trigger modes (<code>TRIG</code>)", " ".join(choices(params, "FM Synth", "LFO1", "TRIG")) + " (the synth's MOD page: the first five)"),
        ("Mod sources (<code>SRC</code>, <code>VIA</code>)", " ".join(router_sources(params)[0])
         + "; then the engine's own: " + ", ".join(f"{short} {' '.join(own)}" for short, own in router_sources(params)[1].items())
         + ". Drums and resonator: none of their own. Synth: LFO3 LFO4 ENV2 ENV3 ENV4 ENV5 after ONE"),
        ("Track input filters (<code>FLT1</code>, <code>FLT2</code>)", " ".join(choices(params, "FM Synth", "XIN", "FLT1"))),
        ("LFO divisions (<code>DIV</code>)", " ".join(divs) + " (D = dotted, T = triplet)"),
        ("Effect sync divisions", " ".join(fxdiv)),
        ("Insert effect types", " ".join(f["name"] for f in params["effects"])),
        ("Drum types", " ".join(choices(params, "Drum", "DR 1", "TYPE"))),
        ("Kick knock kinds, stage orders", " ".join(choices(params, "Drum", "KCK2", "KIND")) + "; " + " ".join(choices(params, "Drum", "KCK2", "PATH"))),
        ("Oscillator bank models", " ".join(choices(params, "Drum", "BANK", "MODL"))),
        ("Resonator drum sounds (<code>SND</code>)", " ".join(choices(params, "Drum", "RSN", "SND"))),
        ("Drum pad waves (<code>W1</code>–<code>W16</code>)", " ".join(c for c in choices(params, "Drum", "WAV1", "W1") if c)),
        ("Resonator bodies (<code>MRPH</code>)", "STRG TUBE COMB PLAT BAR BELL HEAD GONG TAM CRSH RIDE CUP CHNA SPLS (0 to 13, and between)"),
        ("Resonator exciters (<code>XMPH</code>)", "STRK BOW BRTH SCRP BRSH (0 to 4, and between)"),
        ("Reverb shimmer intervals (<code>MODE</code>)", " ".join(fx_choices(params, "RVB", "MODE"))),
        ("Echo read modes (<code>READ</code>)", " ".join(fx_choices(params, "DLY", "READ"))),
        ("Key sources (<code>KEY</code>, DUCK's <code>SRC</code>)", " ".join(fx_choices(params, "CMP", "KEY"))),
        ("Wah filter kinds (<code>FILT</code>)", " ".join(fx_choices(params, "WAH", "FILT"))),
        ("Sampler play modes", " ".join(choices(params, "Sampler", "SAMPL", "MODE"))),
        ("Slice play modes", " ".join(choices(params, "Sampler", "SLICE", "PLAY"))),
        ("Stretch algorithms", " ".join(choices(params, "Sampler", "STRCH", "ALGO"))),
        ("Stretch lengths (bars)", " ".join(choices(params, "Sampler", "STRCH", "LEN"))),
        ("Wavetable scan modes", " ".join(choices(params, "Wavetable", "SCAN", "MODE"))),
        ("Wave sequencer loop modes", " ".join(choices(params, "Wave Sequencer", "SEQ", "LOOP"))),
        ("Wavetable frame options", " ".join(choices(params, "Wavetable", "SCAN", "FRMS"))),
        ("Grain shapes", " ".join(choices(params, "Sampler", "GRAIN", "SHAP"))),
        ("Wavetables", " ".join(choices(params, "Wavetable", "WAVE", "TABL"))),
        ("Macro models", " ".join(m for m, _, _ in ctx["macro_models"])),
        ("FM algorithms", " ".join(html.escape(c) for c in choices(params, "FM Synth", "FMMIX", "ALGO"))),
        ("Trig conditions", "ALL PROB A:B FILL !FIL 1ST !1ST PRE !PRE"),
        ("Arp modes", "; ".join(arp_modes)),
        ("Arp rates", "1/1 1/2 1/4 1/4T 1/8 1/8T 1/16 1/16T 1/32 1/32T 1/64 1/64T (the knob runs slowest to fastest)"),
        ("Roll divisions", "1/2 1/4 1/4T 1/8 1/8T 1/16 1/16T 1/32 1/32T 1/64 1/64T 1/128"),
        ("Track speeds", "1/4 1/2 1x 2x 4x"),
        ("Scale pages (built in)", "Chrom Major Minor Penta Blues Whole Dorian Mixo HrMin mPent Phryg Lydia"),
        ("Preset categories", "ALL INIT BASS PAD LEAD KEYS FX PERC KIT MULT"),
        ("Clock sources", "INT EXT AUTO LINK")], "keys")
    ch.append(("Value lists", [(None, lists)]))

    # =========================================================================================
    terms = table(["Term", "Meaning"], [
        ("Bar screen", "A screen with a picture or list and a strip of eight two-letter knob labels along the bottom."),
        ("Bank", "One of four sets of sixteen pads on CHORD and on PERF; ▲ / ▼ change it."),
        ("Body", "What rings in the resonator: a string, a tube, a plate, a bell, a gong and everything between; also the BODY page of the macro oscillator and the wave sequencer."),
        ("Bus", "One of two shared chains of effects that every track can send to."),
        ("Cell", "One of the eight positions on the display; the knob in the same position edits it."),
        ("Condition", "A rule on a step that decides whether it plays on a given pass."),
        ("Engine", "The sound generator of a track."),
        ("Exciter", "What sets the resonator's body ringing: a strike, a bow, breath, a scrape, a brush."),
        ("Fill", "A held state in which steps marked FILL play."),
        ("Four clicks", "The confirmation for anything destructive: four clockwise clicks on its knob."),
        ("Grain", "A short slice of a sample, played many at a time in granular mode."),
        ("Grid screen", "A screen with eight cells in two rows of four."),
        ("Insert", "An effect in a slot on one track."),
        ("Knob page", "One set of eight cells; the knob-page buttons move between them, going round at the ends."),
        ("Latch", "Notes or an arp carrying on after the keys are released."),
        ("Layer", "One of up to four effects a PERF pad plays at one press."),
        ("Lock (parameter lock)", "A knob value that applies to one step only."),
        ("Micro-timing", "A step's offset from the grid, up to half a step either way."),
        ("Multi preset", "Several connected tracks saved and loaded as one preset (category MULT)."),
        ("Name bar", "The top line of the display."),
        ("Page key", "Func + a top-row pad: jumps straight to a page."),
        ("Pattern", "One of 16 sets of steps for all tracks."),
        ("Pin", "Keeping the DRUMS, CHORD, SCALE or PERF pads playable on every screen."),
        ("Pressure", "Polyphonic aftertouch: how hard each pad is pressed after it is struck, per pad."),
        ("Pressure range", "A range on a knob that a bottom-row pad sweeps with pressure."),
        ("Ratchet", "Several quick repeats inside one step."),
        ("Resampling", "Recording Neptune's own output into a new sample."),
        ("Roll", "A pad repeating while held, faster with pressure."),
        ("Route", "A slot whose source is fixed: a pressure destination and depth on a track (AT ROUTE), one of the strip's three (STRIP) or of the track input's two (XIN)."),
        ("Router", "The one modulation system every sound engine has: sources, destinations and the slots that join them."),
        ("Row", "One line of the song: a pattern, a repeat count and a transposition."),
        ("Send", "The amount of a track fed to a bus."),
        ("Slice", "One piece of a sample, cut evenly or at a hit."),
        ("Slot (mod slot)", "A source, a destination, an amount and a via: one connection of the modulation (SLT pages)."),
        ("Source", "What a slot reads: an LFO, an envelope, velocity, pressure, the strip, the track input, or something the engine offers of its own."),
        ("Sub-block", "The 16 samples (at 48 kHz) over which most modulation is worked out once."),
        ("Take", "The moves of the PERF pads recorded into a pattern."),
        ("Through zero", "A modulated delay whose dry sound is delayed too, so that the two meet and cancel (THRU)."),
        ("Time stretch", "Changing how long a sound lasts without changing its pitch."),
        ("Transient", "The sudden start of a sound, such as a drum hit; where transient slicing cuts."),
        ("Step", "One position in a track's pattern."),
        ("Track input", "Another track's sound, chosen on TRK FX → IN, that this track listens to."),
        ("Track layer", "What is available while ▲ or ▼ is held: track select, mute, and all-track turns."),
        ("Trig", "A step that is switched on.")], "keys")
    ch.append(("Glossary of terms", [(None, terms)]))
    return ch


def ctx_diagram(ctx):
    return f'<div class="diagram">{ctx["controller_svg"]}</div>'
