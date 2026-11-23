"""Long names and descriptions for every short parameter name in the Neptune manual.

DESC maps a short label to a list of (context regex or None, long name, description). The context
string is "<Device>/<PAGE>", e.g. "Synth/FILTR" or "Insert DLY/FX" (an insert effect's cells, the DIRT and
TALK inserts included: "Insert DIRT/FX"). The first matching entry wins,
so put specific contexts before the catch-all (None).

EXTRA lists the cells that do not come from the parameter dump (screens whose cells are built in
the UI code): (short, long, device, page, values, description).

DESTS describes the entries of the LFO DEST lists that are not simply a page cell under its own
name: short -> (long name, engines, description). A numbered family is one entry with '#' for the
number ("ATK#" for ATK1..ATK6). Every DEST entry must be a key of DESC or DESTS (as it is, or with
its number replaced by '#'): the "LFO Destinations" unit tests and chapters.py both check it.
"""

ENV = "envelope"

DESC = {
    # ---- Oscillators (main synth) -------------------------------------------------------------
    "WAV1": [(None, "Oscillator 1 kind", "What oscillator 1 plays: an analog wave (SIN SAW SQR TRI PLS NOIS) or a wavetable (WT), FM pair (FM), formant (FORM) or harmonic (HARM) oscillator.")],
    "WAV2": [(None, "Oscillator 2 kind", "What oscillator 2 plays; the same choices as oscillator 1.")],
    "WAV3": [(None, "Oscillator 3 kind", "What oscillator 3 (the sub oscillator) plays; the same choices as oscillator 1.")],
    "MIX": [
        ("Synth/OSC", "Oscillator mix", "Balance between oscillator 1 and oscillator 2."),
        ("Macro/", "Mix (particle noise model)", "Fourth macro of the DUST model: balance of the noise particles against the tone."),
        (None, "Dry / wet mix", "Balance between the untreated sound (0) and the effect (100)."),
    ],
    "PW": [(None, "Pulse width / shape", "Shapes both oscillators: 50 is the plain wave, and the two directions are two different sounds. Below 50 thins or sharpens: SQR and PLS narrow to a thin pulse, SIN and TRI are wavefolded, SAW gains a second, shifted saw. Above 50 thickens: SQR and PLS gain a square an octave up, SIN and TRI are pushed towards a square, SAW hollows into a square.")],
    "TUN2": [(None, "Oscillator 2 tune", "Pitch of oscillator 2 relative to oscillator 1, in semitones.")],
    "DET2": [(None, "Oscillator 2 detune", "Fine detune of oscillator 2 in cents, for beating and width.")],
    "XMOD": [(None, "Cross-modulation", "How strongly oscillator 2 frequency-modulates oscillator 1: through-zero linear FM, so the note stays in tune and whole-number tunings of oscillator 2 (TUN2 at 0, 12, 19, 24) give harmonic tones at any depth; other tunings give bells and clangs.")],
    "DRIV": [
        ("Synth/OSC", "Mixer drive", "Overdrives the oscillator mix before the filter."),
        ("(Synth|FM|Macro|Wavetable|Wave seq|Resonator)/FILTR", "Filter drive", "Input drive into the filter. Each type overdrives in its own way; the SVF soft-clips its input, and on COMB it darkens the feedback."),
        ("Drums/", "Drive", "Saturation of this drum voice, on every type. The snares also shift from body to noise with it, CLAP adds claps, SHKR rattles deeper."),
        ("Sampler/", "Drive", "Saturation of the sampler voice."),
        ("Insert (DLY|LCR|MTAP|RVSE)/", "Loop drive", "How hard the repeats are pushed into the echo's saturation (SAT); 0 is a clean echo."),
        (None, "Drive", "Amount of distortion."),
    ],
    "TUN3": [(None, "Oscillator 3 tune", "Pitch of oscillator 3 in semitones.")],
    "LVL3": [
        ("Synth/", "Oscillator 3 level", "Level of oscillator 3 (sub)."),
        (None, "Operator level", "Output level of the operator shown by the number (FMMIX page: one knob per operator)."),
    ],
    "NOIS": [
        ("Insert VOC/", "Unvoiced noise", "Vocoder: noise added to this track's sound, so that the input's consonants have something to shape."),
        (None, "Noise level", "Level of the noise source."),
    ],
    "NCOL": [(None, "Noise colour", "Tone of the noise, from dark to bright.")],
    "TUNE": [
        ("Drums/", "Tune", "Pitch of the drum voice in Hz: on a kick the pitch it settles on, on RSN the pitch of the body; on BANK the size of the instrument: 8000 is the model's own pitch, 4000 an octave lower."),
        ("Insert DIRT/", "Feedback tune", "Frequency of the DIRT insert's feedback path."),
        (None, "Tune", "Coarse pitch of the whole sound in semitones."),
    ],
    "FINE": [(None, "Fine tune", "Fine pitch of the whole sound.")],
    "BEND": [(None, "Bend range", "How far the pitch strip bends the sound, in semitones.")],

    # ---- Voice ---------------------------------------------------------------------------------
    "ARCH": [(None, "Architecture", "Loads a starting voice architecture into the same pages: INIT, MONO, HYBR (hybrid) or POLY. Each one keeps what you dial in on it: switch away and back and the sound is as you left it.")],
    "MODE": [
        ('Sampler/SLICE', 'Slice mode', 'GRID: equal slices. TRNS: one slice per detected hit (transient).'),
        ('Wavetable/SCAN', 'Scan mode', 'Moves the table position by itself over a set time: OFF, RUN (once, then holds), LOOP, PING (there and back), STEP (in audible steps), RAND (random frames).'),
        ("Synth/VOICE", "Voice mode", "MONO, DUO (two voices), POLY, or UNI (all voices stacked on one note)."),
        ("Sampler/SAMPL", "Play mode", "FWD forward, REV reverse, 1SHT one-shot, GATE while held, LOOP, PING back-and-forth loop, GRAN granular."),
        ("Insert DLY/", "Delay mode", "STER stereo, PING ping-pong, TAPE with tape-style tone."),
        ("Insert (RVB|HALL|SPRG|SHIM)/", "Shimmer interval", "What SHIM feeds back into the tail: OCT+ an octave up, 5TH+ a fifth up, O+5 an octave and a fifth up, OCT- an octave down, REV played backwards, SUB two octaves down. HOLD is no interval: the reverb is held (its way in shuts and the tail goes round until MODE is moved on)."),
        ("Insert VOC/", "Vocoder mode", "BAND: the classic vocoder, this track's sound through sixteen bands that follow the input's. TUNE: this track's sound drives a tuned body (BODY) at the pitch of the track's last note, shaped by the input's bands, ringing on for RING."),
        ("ARP/ARP", "Arp mode", "Order the held notes are played in (15 modes; Appendix C)."),
        ("ARP/MUTA", "Mutate mode", "ANC: every mutation starts again from the ancestor. CUM: mutations build on each other."),
        ("ARP/ROLL", "Roll mode", "QUANT: repeat rates are tempo divisions. FREE: rates in Hz."),
        (None, "Filter mode", "Response of the chosen filter type (low-pass, band-pass, high-pass, notch, slopes); the list depends on TYPE."),
    ],
    "VOX": [(None, "Voices", "How many notes can sound at once, 1 to 16 (8 to start with). At 1 the engine is mono, the last note played having the say. Lowered under held notes, the notes too many are released, not cut.")],
    "LEGA": [(None, "Legato", "On: overlapping notes do not retrigger the envelopes.")],
    "STCK": [(None, "Unison stack", "Number of detuned copies stacked on every note (off, 2, 4, 8).")],
    "SPRD": [
        ('Sampler/STRCH', 'Stereo spread', "Spreads the stretch's grains across left and right (GRAN and SMER)."),
        ("Synth/", "Unison spread", "Detune between the stacked unison voices."),
        ("Macro/", "Spread (virtual analog model)", "Second macro of the VA model: detune between the stacked saws."),
        ("Wavetable/", "Spread", "Detune and width of the wavetable's doubled voices."),
        ("Sampler/GRAIN", "Grain spread", "Stereo spread of the grains."),
        (None, "Stereo spread", "How far the effect is spread across left and right."),
    ],
    "GLID": [
        ("ARP/ARP", "Rate glide", "Time, as a division, over which the arp slides to a new rate."),
        ("ARP/ROL2", "Division glide", "Time over which a roll slides between repeat rates (quantized mode)."),
        (None, "Glide", "Portamento time between notes."),
    ],
    "GLMD": [(None, "Glide mode", "ALL: every note glides. LEGA: only overlapping notes glide.")],

    # ---- Analog character ----------------------------------------------------------------------
    "DRFT": [(None, "Drift amount", "Slow random wander of each oscillator's pitch.")],
    "DRAT": [(None, "Drift rate", "Speed of the pitch drift.")],
    "SLOP": [(None, "Tuning slop", "Each new note starts slightly out of tune by up to this many cents.")],
    "PHAS": [
        ("Synth/ANALG", "Random phase", "On: oscillators start at a random point in their cycle on each note."),
        ("Insert PAN/", "Stereo phase", "Phase offset between left and right of the auto-pan."),
        (None, "Start phase", "Point in its cycle at which the LFO starts."),
    ],
    "WSHP": [(None, "Waveshape variation", "Random change of the waveform on every note: a component tolerance at low settings, a difference heard from note to note towards the top.")],
    "SOFT": [(None, "Soft start", "Pitch overshoot at the start of each note, in cents.")],
    "FVAR": [(None, "Filter variation", "Random cutoff offset per note.")],
    "EVAR": [(None, "Envelope variation", "Random change of envelope times per note.")],

    # ---- Filter --------------------------------------------------------------------------------
    "CUT": [
        ("MIDI track/", "Cutoff (CC 74)", "Sends the filter cutoff controller to the external instrument."),
        (None, "Cutoff", "Filter cutoff frequency."),
    ],
    "RESO": [
        ("MIDI track/", "Resonance (CC 71)", "Sends the resonance controller to the external instrument."),
        ("Insert TALK/", "Formant resonance", "Sharpness of the vowel formants."),
        (None, "Resonance", "Emphasis at the cutoff frequency. The LADR type self-oscillates at full resonance."),
    ],
    "ENV": [
        ("Insert (FLT|PHS)/", "Level to sweep", "How far the level of what KEY listens to moves the cutoff (FLT, up to five octaves) or the sweep (PHS): up to the right of the middle, down to the left. An envelope follower: with KEY on another track, that track opens this filter."),
        ("Insert TALK/", "Envelope to vowel", "How far the playing level pushes the vowel; negative pushes it the other way."),
        ("Synth/MENV", "Envelope select", "Which extra envelope (ENV2 to ENV5) this page edits."),
        (None, "Envelope amount", "How far the filter envelope moves the cutoff; negative closes the filter. Up to 7 octaves."),
    ],
    "TYPE": [
        ("Drums/", "Drum type", "What this pad is (10 types; Appendix C). FMKK, KICK, MKIK and KSYN are the kick machine at four starting points (pages KICK and KCK2); CLAP, SHKR, SUB and CYMB are small voices of their own; RSN is the resonator drum (page RSN: toms, snares, cymbals, bells, gongs); BANK is the oscillator bank (page BANK: hats and analog cymbals). Turning to one of the three kicks loads that kick into the KICK and KCK2 pages."),
        ("Insert DIRT/", "Distortion curve", "One of the eighteen saturation curves (Chapter 8): SOFT, HARD, FOLD, ASYM, then TANH to CHB3."),
        ("TRK FX/FX", "Insert effect type", "Which effect is in the selected insert slot (33 types; Appendix C). Changing it replaces the effect and its values."),
        (None, "Filter type", "Filter model: SVF, SEM, LADR (ladder), DIOD (diode), COMB, or OFF."),
    ],
    "MORP": [
        ("Macro/MACRO", "Morph", "Fourth macro of the VA, FORM and PLCK models. VA: saw to square. FORM: voice to whisper (noise takes over from the pulse train). PLCK: string to drum (the pitch dissolves into a burst)."),
        (None, "Filter morph", "On the SEM type in MRPH mode: sweeps low-pass through notch to high-pass. Anywhere else the knob says SEM MRPH ONLY."),
    ],
    "FFM": [(None, "Filter FM", "Oscillator 2 modulates the cutoff at audio rate.")],
    "KTRK": [
        ("Synth/MOD2", "Key track to LFO rate", "Higher notes speed the LFO up (not while SYNC is on)."),
        (None, "Key tracking", "How far the cutoff follows the played note, -100 to +100: at +100 it rises an octave per octave played (on the sampler a tenth of its range), at 0 it stays put, below 0 it falls as the notes rise. Middle C is the centre."),
    ],
    "TYPB": [(None, "Filter B type", "Model of the second filter.")],
    "MODB": [(None, "Filter B mode", "Response of the second filter.")],
    "OFSB": [(None, "Filter B offset", "Cutoff of filter B relative to filter A, in semitones.")],
    "RESB": [(None, "Filter B resonance", "Resonance of the second filter.")],
    "ROUT": [(None, "Filter routing", "SNGL: filter A only. SER: A into B. PAR: A and B side by side.")],
    "BAL": [(None, "Filter balance", "Balance between filter A and filter B.")],
    "DRVB": [(None, "Filter B drive", "Input drive into filter B.")],
    "TRKB": [(None, "Filter B key tracking", "How far filter B's cutoff follows the note.")],
    "VELF": [(None, "Velocity to filter", "Harder notes open the filter further.")],
    "ENVF": [(None, "Envelope to filter", "How far the amp envelope moves the cutoff; negative closes it.")],
    "FDRV": [(None, "Filter drive", "Input drive into the sampler's filter.")],

    # ---- Envelopes -----------------------------------------------------------------------------
    "ATK": [
        ("Insert (CMP|DUCK)/", "Attack", "How fast the gain is taken down, in milliseconds."),
        ("Insert TRNS/", "Attack gain", "Boosts or softens the start of each hit."),
        ("MIDI track/", "Attack (CC 73)", "Sends the attack-time controller."),
        (None, "Attack", "Time the envelope takes to rise to full level."),
    ],
    "DEC": [
        ("Drums/", "Decay", "Length of the drum voice. A drum rings for its DEC however short the pad press or the step; SUB sounds while held and fades over DEC afterwards."),
        ("MIDI track/", "Decay (CC 75)", "Sends the decay-time controller."),
        (None, "Decay", "Time the envelope takes to fall to the sustain level."),
    ],
    "SUS": [
        ("Insert TRNS/", "Sustain gain", "Boosts or reduces the tail after each hit."),
        (None, "Sustain", "Level held while the note is down."),
    ],
    "REL": [
        ("Insert LMT/", "Release", "How fast the limiter recovers, in milliseconds."),
        ("Insert (CMP|DUCK)/", "Release", "How fast the gain comes back, in milliseconds."),
        ("Insert VOC/", "Band release", "Vocoder: how fast a band falls after the input leaves it, in milliseconds."),
        ("MIDI track/", "Release (CC 72)", "Sends the release-time controller."),
        (None, "Release", "Time the envelope takes to fall to silence after the note ends."),
    ],
    "CURV": [
        ("Insert WSH/", "Shaping curve", "One of the eighteen saturation curves (Chapter 8): SOFT, HARD, FOLD, ASYM, then TANH to CHB3."),
        (None, "Envelope curve", "Shape of the envelope segments, from logarithmic through linear to exponential."),
    ],
    "KEY": [
        ("Insert ", "Key source", "What the effect listens to: SELF (the sound it works on), a track T1 to T16, or IN (the track's input, TRK FX page IN). A compressor keyed from the kick ducks; a filter's ENV or an LFO's TRIG follows the hits of the keyed track. Works on a send bus too."),
        ("Sampler/", "Key follow", "On: pitch follows the note. Off: every note plays the sample at its original pitch."),
        (None, "Key tracking", "How far the cutoff follows the played note, -100 to +100: at +100 it rises an octave per octave played (on the sampler a tenth of its range), at 0 it stays put, below 0 it falls as the notes rise. Middle C is the centre."),
    ],
    "VEL": [
        (".*/F ENV", "Velocity to cutoff", "Harder notes open the filter further."),
        ("Synth/MOD2", "Velocity to LFO depth", "Note velocity adds to (or, negative, subtracts from) the LFO's depth."),
        ("ARP/PLAY", "Strum velocity", "How much the velocity changes across a strummed chord."),
        ("TRK FX/ARPLK", "Arp velocity lock", "Per-step lock of the arpeggiator's velocity."),
        ("STEP/TRIG", "Velocity", "Velocity of the notes on this step (0 to 127)."),
        (None, "Velocity sensitivity", "0: every note at full level. 100: level follows how hard you play."),
    ],
    "PMOD": [(None, "Poly-mod", "Filter envelope to oscillator 2 pitch, up to two octaves either way.")],
    "EVEL": [(None, "Velocity to envelope amount", "Harder notes increase the filter envelope's reach.")],
    "WHL": [
        ("Synth/MOD2", "Mod strip to LFO depth", "The mod strip adds to the LFO's depth; set DPTH to 0 for vibrato from the strip only."),
        (None, "Mod strip to cutoff", "The mod strip opens (or, negative, closes) the filter."),
    ],
    "LEVL": [
        ("Drums/", "Level", "Level of this drum voice."),
        ("FM/OP", "Operator level", "Output level of this operator: loudness for a carrier, modulation depth for a modulator."),
        ("Insert AMP/", "Output level", "Output level of the amp effect in dB."),
        (None, "Level", "Output level of the engine, before the track's mixer volume."),
    ],
    "CHAN": [
        ("MIDI track/", "MIDI channel", "Channel the track sends on."),
        (None, "Envelope chain", "OFF, SER (stages follow each other) or PAR (stages run together)."),
    ],
    "STGS": [
        ("Insert PHS/", "Stages", "Number of phaser stages."),
        (None, "Chain stages", "Number of envelopes in the chain."),
    ],
    "COMB": [(None, "Chain combine", "How parallel envelope stages are combined: SUM or MAX.")],

    # ---- FM ------------------------------------------------------------------------------------
    "LVL#": [(None, "Operator level", "FMMIX page: output level of operators 1 to 6, one knob each.")],
    "ALGO": [
        ('Sampler/', 'Stretch algorithm', 'OFF, TAPE (pitch follows speed), CYCL (vintage cyclic stretch, metallic), GRAN (smooth granular), SLCE (slices keep their sound, only their timing moves), SMER (extreme smear), WSOL (clean stretch).'),
        ("Insert DRV/", "Drive algorithm", "SOFT, HARD, FOLD or CRSH (bit-crush)."),
        ("Wavetable/", "Key-frame algorithm", "How the frames between two keys of the KEYS table are made. LIN: a crossfade. STEP: none, the nearer key. CLSC: a crossfade kept as 128 samples of 8 bits. PHAS: level and phase of each harmonic separately, so nothing cancels on the way. TRNS: the energy glides along the harmonics (optimal transport), so a formant slides instead of fading. VOC: first the outline of the spectrum changes, then its harmonics; half way it is one key's formants on the other's harmonics."),
        (None, "Algorithm", "How the six operators are wired (8 algorithms, from one chain of six to six parallel carriers)."),
    ],
    "RAT#": [(None, "Operator ratio", "RATIO page: frequency ratio of operators 1 to 6, one knob each. A turn steps through the harmonics (0.5, 1, 2, 3 ... 16); Shift + turn moves by 0.01 for in-between ratios; Func + turn by an octave.")],
    "RATI": [
        ("FM/", "Operator ratio", "Frequency of this operator as a multiple of the note."),
        ("Macro/", "Ratio (FM model)", "Second macro of the FM model: modulator ratio."),
        (None, "Compression ratio", "How strongly level above the threshold is reduced."),
    ],
    "DETN": [
        ("Macro/", "Detune (FM model)", "Third macro of the FM model: detunes the modulator up to a semitone either way of 50; the tone beats and turns inharmonic."),
        (None, "Operator detune", "Fine detune of this operator in cents."),
    ],
    "FDBK": [
        ("FM/", "Operator feedback", "The operator modulates itself; adds bite, then noise."),
        ("Macro/", "Feedback (plucked string and FM models)", "Third macro of the PLCK model: string feedback (sustain). Fourth macro of the FM model: the modulator feeds back into itself; bite, then noise."),
        ("Insert DIRT/", "Feedback", "Tuned feedback around the distortion; howls at high settings."),
        (None, "Feedback", "How much of the effect's output is fed back into it."),
    ],

    # ---- Macro oscillator ----------------------------------------------------------------------
    "MODL": [
        ("Drums/", "Bank model", "Oscillator bank: which instrument. HAT a closed hat; OHAT an open hat (open for as long as the pad is held); METL six square oscillators through two bands, the analog drum-machine cymbal; RING the same multiplied in pairs."),
        (None, "Model", "Synthesis model: VA, FM, FOLD, FORM, HARM, PLCK, DUST, BOW. Knobs 2 to 5 are renamed to suit the model."),
    ],
    "SAWS": [(None, "Saws (virtual analog model)", "First macro of the VA model: number and blend of stacked saws.")],
    "HARM": [("Wavetable/", "Highest harmonic", "The highest harmonic kept of every key of the KEYS table, 1 to 127."),
             (None, "Harmonics (virtual analog model)", "Third macro of the VA model. 50 is the saws as they are; below, each fades into a sine; above, a saw an octave up is mixed in."),
    ],
    "BIAS": [("Wavetable/", "Key bias", "Leans the frames between two keys towards the earlier key (down) or the later one (up)."),
             (None, "Bias (wavefolder model)", "Third macro of the FOLD model: pushes the wave off-centre into the folder, either way of 50, which brings in the even harmonics."),
    ],
    "STRC": [(None, "Stretch (harmonic model)", "Fourth macro of the HARM model: spreads the harmonics apart, as on a stiff string or a bar; the tone turns into a bell.")],
    "RING": [
        ("Insert VOC/", "Ring time", "Vocoder, TUNE mode: the time the body's first partial takes to fall by 60 dB, 0.02–10 s."),
        (None, "Ring (particle noise model)", "Third macro of the DUST model, neutral at 50. Above: the particles ring at the pitch of the key. Below: a high-pass thins them into a hiss that follows the key."),
    ],
    "INDX": [(None, "Index (FM model)", "First macro of the FM model: modulation depth.")],
    "FOLD": [(None, "Fold (wavefolder model)", "First macro of the FOLD model: fold amount.")],
    "SHPE": [
        ("Macro/", "Shape (wavefolder model)", "Second macro of the FOLD model: input waveform."),
        ("Insert", "LFO shape", "Waveform of the effect's own LFO."),
        (None, "LFO shape", "LFO waveform: SIN TRI SQR SAW RND (stepped random) SRND (smooth random) RAMP EXPU EXPD."),
    ],
    "CHEB": [(None, "Chebyshev (wavefolder model)", "Fourth macro of the FOLD model: harmonic-selective shaping.")],
    "VOWL": [
        ("Macro/", "Vowel (formant model)", "First macro of the FORM model: vowel."),
        (None, "Vowel", "Position through the vowels A, E, I, O, U."),
    ],
    "SHFT": [
        ("Macro/", "Shift (formant model)", "Second macro of the FORM model: formant shift."),
        (None, "Formant shift", "Moves all formants down or up: larger or smaller mouth."),
    ],
    "Q": [(None, "Q (formant model)", "Third macro of the FORM model: formant sharpness.")],
    "TILT": [
        ("Wavetable/", "Key tilt", "Makes every key of the KEYS table darker (down) or brighter (up), by up to 6 dB per octave."),
        (None, "Tilt (harmonic model)", "First macro of the HARM model: spectral tilt."),
    ],
    "EVOD": [(None, "Even / odd (harmonic model)", "Second macro of the HARM model: balance of even and odd harmonics.")],
    "NUM": [(None, "Number (harmonic model)", "Third macro of the HARM model: number of harmonics.")],
    "DAMP": [
        ("Macro/", "Damping (string models)", "Macro of the PLCK and BOW models: string damping."),
        ("Drums/", "Bank damping", "Oscillator bank: how much sooner the highs die than the lows. 0: everything rings equally long; higher: the top is gone first, as on a real cymbal."),
        ("Insert DIRT/", "Damping", "Darkens the DIRT insert's feedback."),
        (None, "Damping", "How quickly high frequencies die away in the tail."),
    ],
    "PICK": [(None, "Pick (plucked string model)", "Second macro of the PLCK model: pick position.")],
    "DENS": [
        ("Macro/", "Density (particle noise model)", "First macro of the DUST model: particle density."),
        ("ARP/", "Density", "Variation macro: how many of the arp's steps actually sound."),
        (None, "Grain density", "Grains started per second."),
    ],
    "COLR": [
        ("Drums/", "Bank colour", "Oscillator bank: where the sound is strongest, from dark through the model's own (50) to bright: it moves the two bands the squares pass."),
        (None, "Colour (particle noise model)", "Second macro of the DUST model: tone of the particles."),
    ],
    "PRES": [
        ("Macro/", "Pressure (bowed string model)", "First macro of the BOW model: bow pressure, from a light, glassy bow to a pressed, rough one."),
        (None, "Pressure to LFO depth", "The voice's own pad pressure adds to the LFO's depth."),
    ],
    "POS": [
        ("Macro/", "Position (bowed string model)", "Second macro of the BOW model: bow position."),
        (None, "Table position", "Where in the wavetable the oscillator reads; sweeping it morphs the tone."),
    ],
    "SPD": [
        ('Sampler/', 'Stretch speed', 'Playback speed with the pitch kept (except TAPE): 0.25× to 4×. In SMER the speed is a sixteenth of the value shown.'),
        ('Wavetable/', 'Scan speed', "Divides the scan's pass time: 2× runs through twice as fast."),
        ("Macro/", "Speed (bowed string model)", "Fourth macro of the BOW model: bow speed."),
        ("Insert ROT/", "Speed", "Rotary speaker speed, slow to fast."),
        ("Insert TRNS/", "Detector speed", "How quickly the transient shaper reacts."),
        (None, "Track speed", "Playback speed of the track against the tempo: 1/4, 1/2, 1x, 2x, 4x."),
    ],

    # ---- Wavetable -----------------------------------------------------------------------------
    "TABL": [(None, "Table", "Which wavetable: 23 built-in tables, USER (a sample cut into frames) or KEYS (a few waves picked out of samples, the frames between them computed).")],
    "MSPD": [(None, "Motion speed", "Speed of the built-in sweep of the table position.")],
    "MDPT": [(None, "Motion depth", "Depth of the built-in position sweep.")],
    "ENVP": [(None, "Envelope to position", "The amp envelope moves the table position.")],
    "VELP": [(None, "Velocity to position", "Harder notes start further into the table.")],
    "INTP": [(None, "Interpolation", "How the table is read: LIN smooth, SPEC spectral, STEP stepped, VINT vintage (gritty), CLSC classic: as an early digital wavetable synth read its tables (64 positions, waves of 128 samples and 8 bits, nothing smoothed or band-limited), which gives high notes a glassy, aliased top.")],
    "KFRM": [(None, "Key table frames", "How many frames the KEYS table has: 8 to 256. Few frames and INTP on STEP or CLSC make the steps of a scan audible.")],
    "WRAP": [(None, "Wrap", "On: after the last key the KEYS table returns to the first, so a looped scan has no jump.")],
    "ALGN": [(None, "Align", "On: every key is turned in time until its fundamental starts like a sine, so waves cut from different places do not cancel each other. Off: as they were cut.")],
    "SMP#": [(None, "Key sample", "K SMP page: the sample key frame 1 to 8 is taken from. OFF: the key is not used.")],
    "POS#": [(None, "Key position", "K POS page: where in its sample key frame 1 to 8 is taken. One cycle is cut there (one period where the sample has a pitch) and plays at the pitch of the keys.")],
    "PLC#": [(None, "Key place", "K PLC page: where along the table key frame 1 to 8 sits. Only order and spacing count: the earliest key in use is the first frame, the latest the last.")],
    "OCT": [
        ("ARP/", "Octave range", "Number of octaves the arp spans."),
        (None, "Octave", "Octave shift of the oscillator."),
    ],
    "VIBR": [(None, "Vibrato", "Depth of pitch vibrato.")],
    "SUB": [(None, "Sub level", "Level of the sub oscillator one octave down.")],

    # ---- Wave sequencer ------------------------------------------------------------------------
    "LEN": [
        ('Sampler/', 'Stretch length', 'With SYNC on, how long the loop lasts: 1/16 of a bar to 32 bars.'),
        ('Wavetable/', 'Scan length', 'With SYNC on, how long one pass lasts: 1/16 to 16 bars.'),
        ("Wave seq/", "Sequence length", "Number of steps in the wave sequence."),
        ("STEP/TRIG", "Note length", "Length of the step's note, in steps."),
        (None, "Track length", "Number of steps in this track's pattern, 1 to 1024, independent per track."),
    ],
    "XFAD": [
        ('Sampler/', 'Crossfade', 'Overlap between successive pieces of the stretch; in SLCE, how quickly a slice is faded when the next arrives.'),
        (None, "Crossfade", "How much each step's wave blends into the next."),
    ],
    "LOOP": [(None, "Loop mode", "FWD, REV, PING, RAND or ONCE.")],
    "RATE": [
        ("Wave seq/", "Rate", "Speed of the wave sequence relative to the tempo."),
        ("Insert DIRT/", "Sample-rate reduction", "Lowers the sample rate for aliasing grit."),
        ("Insert RED/", "Sample-rate reduction", "Lowers the sample rate for aliasing grit."),
        ("Sampler/TONE", "Sample-rate reduction", "Lowers the playback sample rate for aliasing grit."),
        ("Insert TALK/", "Vowel LFO rate", "Speed of the vowel movement."),
        ("ARP/", "Arp rate", "Note value of the arp's steps (Appendix C)."),
        (None, "Rate", "Speed of the LFO or effect modulation. With SYNC on it picks a tempo division instead."),
    ],
    "PRNG": [(None, "Pitch range", "Scales how far the pitch lane reaches (0 = flat, 2 = double).")],
    "TLEN": [(None, "Time lane length", "Length of the step-duration lane.")],
    "PLEN": [(None, "Pitch lane length", "Length of the pitch lane.")],
    "SLEN": [
        ('Sampler/', 'Slice length', 'Shortens every slice to this share of its length, for gated, choppy playback.'),
        (None, "Shape lane length", "Length of the wave-shape lane."),
    ],
    "LLEN": [(None, "Level lane length", "Length of the level lane. Lanes of different lengths drift against each other.")],
    "SYNC": [
        ('Sampler/STRCH', 'Stretch tempo sync', 'On: the loop is stretched to last exactly LEN at the current tempo, and follows tempo changes.'),
        ('Wavetable/SCAN', 'Scan sync', 'OFF: the pass lasts TIME. ON: it lasts LEN at the tempo. NAT: it lasts as long as the original recording (USER table).'),
        ("Wave seq/", "Tempo sync", "On: step times follow the tempo."),
        ("TRK FX/FX", "Effect tempo sync", "Shown in place of ON for effects with a TIME or RATE: that cell then picks a division from 1/128 to 16 bars."),
        (None, "LFO tempo sync", "On: RATE (or DIV) is a tempo division instead of Hz."),
    ],
    "SLID": [(None, "Slide", "On: lane values glide between steps instead of jumping.")],
    "SHP*": [(None, "Shape, all steps", "Sets every step's wave at once.")],
    "P#": [(None, "Step pitch", "PIT A / PIT B pages: pitch offset of steps 1 to 16, one knob per step.")],
    "S#": [(None, "Step shape", "SHP A / SHP B pages: waveform of steps 1 to 16.")],
    "L#": [(None, "Step level", "LVL A / LVL B pages: level of steps 1 to 16.")],
    "T#": [(None, "Step time", "TIM A / TIM B pages: duration of steps 1 to 16.")],

    # ---- Sampler -------------------------------------------------------------------------------
    "SMPL": [(None, "Sample", "Which sample the track plays: factory hits and loop, your files, the drum breaks (B080 … B170, named by tempo), and resampled takes. Choosing a break sets LEN to its length in bars.")],
    "STRT": [(None, "Start", "Where playback starts in the sample.")],
    "END": [(None, "End", "Where playback ends.")],
    "LPST": [(None, "Loop start", "Start of the looped region.")],
    "LPEN": [(None, "Loop end", "End of the looped region.")],
    "SLCS": [
        (None, "Slices", "GRID mode: number of equal slices. TRNS mode: shows how many hits were found; turning it down keeps only the strongest (1 = no limit, up to 64)."),
        (None, "Slices", "Number of equal slices the sample is cut into."),
    ],
    "SLIC": [(None, "Slice select", "Which slice plays.")],
    "SIZE": [
        ('Sampler/STRCH', 'Cycle / grain size', 'Length of the pieces the stretch works in. Short is buzzy and tight, long is smooth and smeared.'),
        ("Sampler/", "Grain size", "Length of each grain."),
        (None, "Room size", "Size of the reverb space."),
    ],
    "SCAN": [(None, "Scan speed", "How fast the grain read position moves through the sample, independent of pitch. 0 freezes; negative runs backwards.")],
    "SPRY": [(None, "Spray", "Random scatter of grain positions and timing.")],
    "PJIT": [(None, "Pitch jitter", "Random pitch per grain, up to an octave either way.")],
    "SHAP": [
        ("Drums/", "Kick shape", "Kick machine: the body's waveform, from a sine through a triangle to a squared-off wave."),
        (None, "Grain shape", "Grain window: HANN smooth, TRI, PERC percussive, SWEL swelling."),
    ],
    "RVRS": [(None, "Reverse chance", "Proportion of grains played backwards.")],
    "PTCH": [(None, "Pitch", "Transposition of the sample in semitones.")],
    "BITS": [(None, "Bit reduction", "Lowers the bit depth for digital grit.")],
    "BIP": [("AT ROUTE/", "Bipolar route", "AT ROUTE, second page: the pad is read both ways. Half pressure leaves the destination where it is, less moves it against the depth, more with it. A pad at rest still does nothing. Not on a drum kit's own destinations.")],
    "TONE": [
        ("Drums/", "Tone", "Brightness, in the way of each type: the width of CLAP's band, the high-pass of CYMB, a broad hiss to a pitched rattle on SHKR, sine to square on SUB, a low-pass on the kick machine (from 200 Hz, or from the kick's own pitch with LPKT up), the tilt of the resonator drum, the level of everything above about 2.5 kHz on the oscillator bank."),
        (None, "Tone", "Brightness of the effect."),
    ],
    "PAN": [
        ("MIDI track/", "Pan (CC 10)", "Sends the pan controller."),
        (None, "Pan", "Left / right position."),
    ],
    "RAND": [(None, "Random start", "Each note starts at a random point up to this far into the sample.")],

    "ROOT": [
        (None, 'Root note', 'The key at which the sample plays at its original pitch; in KEYS slice play, the key that plays slice 1.'),
    ],
    "NORM": [
        ("Wavetable/", "Frame level", "Level of the frames of the KEYS table. OFF: as computed. PEAK: every frame at full scale. RMS: every frame equally loud."),
        (None, 'Normalise', "On: the sample's loudest point is brought to full level."),
    ],
    "GAP": [
        (None, 'Minimum gap', 'Shortest time allowed between two detected hits; raise it to merge flams and rolls into one slice.'),
    ],
    "PLAY": [
        (None, 'Slice play', 'ONE: the slice chosen by SLIC. KEYS: each key plays its own slice, upward from ROOT. NEXT: each new note plays the next slice. RAND: a random slice per note.'),
    ],
    "JIT": [
        (None, 'Jitter', 'Randomises where each cycle or grain starts; smooths the buzz of CYCL, widens the blur of SMER.'),
    ],
    "STEP": [
        (None, 'Scan steps', 'Number of plateaus (STEP mode) or of new random frames per pass (RAND mode).'),
    ],
    "DIR": [
        ('Wavetable/', 'Scan direction', 'FWD runs from POS to the end of the table; REV from the end back to POS.'),
        (None, 'Strum direction', 'Direction of the strum.'),
    ],
    "FRMS": [
        (None, 'Frames', "How a sample is cut into the USER table: 64, 128 or 256 equal pieces, or A64 / A128 / A256, aligned to the sound's pitch period so scanning is smooth and plays in tune."),
    ],
    # ---- Drums ---------------------------------------------------------------------------------
    "SNAP": [(None, "Snap", "The front edge of the hit. 50 leaves the drum as it is; below, the onset is softened; above, the first milliseconds are lifted and the tonal drums get a click. The kick machine sets its attack time with it (EDGE on KCK2 is the other drums' front edge for a kick), CLAP also the length of each clap. On RSN and BANK it is what strikes: a soft mallet (0), a stick (50), a hard tip (100).")],
    # ---- Kick machine (drum type KSYN) -----------------------------------------------------------
    "SWEP": [(None, "Kick sweep", "Kick machine: how far above TUNE the pitch starts, in semitones.")],
    "SWPT": [(None, "Kick sweep time", "Kick machine: how long the pitch takes to fall to TUNE.")],
    "SCRV": [(None, "Kick sweep curve", "Kick machine: where in the sweep time the fall happens; negative = nearly all at the start.")],
    "KNCK": [(None, "Knock", "Kick machine: level of the short tick at the very start of the drum.")],
    "KTON": [(None, "Knock tone", "Kick machine: from a dull thud of about 12 ms to a bright tick of about 1.5 ms.")],
    "HOLD": [
        ("Insert (RVB|HALL)/", "Hold", "On: the reverb's way in shuts and its tail loses nothing: what is in it goes round until HOLD is switched off (the same as MODE at HOLD)."),
        ("Insert DUCK/", "Hold time", "How long the track stays down after the source has fallen under the threshold, 0–500 ms, before REL lets it come back."),
        (None, "Kick hold", "Kick machine: time at full level before the decay starts."),
    ],
    "ACRV": [(None, "Kick decay curve", "Kick machine: shape of the decay; negative falls fast then lingers.")],
    # ---- Oscillator bank (drum type BANK); its MODL, COLR, NOIS and DAMP are above ---------------
    "PING": [(None, "Band balance", "Oscillator bank: the balance of its two bands, the lower one against the upper one; 50 is both alike.")],
    "BLOM": [(None, "Bloom", "Oscillator bank: the highs arrive after the stroke and louder, as the energy of a hard stroke climbs into them.")],
    "LOFI": [(None, "Lo-fi", "Oscillator bank: the grain of an early sampled drum-machine cymbal: the sound is read out at a low rate with few steps of level (0 = off).")],

    # ---- LFO pages -----------------------------------------------------------------------------
    "DPTH": [
        ("ARP/ACFG", "Pressure depth", "How far pad pressure moves the arp's pressure destination."),
        ("ARP/ROLL", "Pressure depth", "How far pad pressure raises the repeat rate from BASE toward MAX."),
        ("AT ROUTE/", "Route depth", "How far pressure moves this route's destination; negative inverts. On a drum kit's own destinations 0 to 100."),
        ("Synth/MENV", "Envelope depth", "How far the extra envelope moves its destination; negative inverts."),
        ("Insert TALK/", "Vowel LFO depth", "How far the vowel moves by itself."),
        (None, "Depth", "Amount of modulation. On LFOs negative values invert the shape."),
    ],
    "DIV": [(None, "LFO division", "The tempo division used while SYNC is on: 22 values from 16 bars to 1/128, with dotted and triplet ones (Appendix C).")],
    "FADE": [
        ('Sampler/SLICE', 'Slice fade', 'Fades the end of each slice to avoid clicks.'),
        ("Insert VIB/", "Fade-in", "Time the vibrato takes to come in."),
        (None, "LFO fade-in", "Time the LFO takes to reach full depth after a note starts."),
    ],
    "DST1": [(None, "Strip route 1 destination", "STRIP page: what the mod strip moves on route 1. The same list as the LFOs' DEST (Appendix C); from the strip, LRAT and LDPT act on both LFOs. Func + turn goes from group to group.")],
    "AMT1": [(None, "Strip route 1 amount", "STRIP page: how far the strip, fully up, moves the destination of route 1, in that destination's own scale: +100 is what an LFO at full depth reaches at its peak. Negative moves it the other way; 0 switches the route off.")],
    "DST2": [(None, "Strip route 2 destination", "STRIP page: what the mod strip moves on route 2. The same list as the LFOs' DEST (Appendix C); from the strip, LRAT and LDPT act on both LFOs. Func + turn goes from group to group.")],
    "AMT2": [(None, "Strip route 2 amount", "STRIP page: how far the strip, fully up, moves the destination of route 2, in that destination's own scale: +100 is what an LFO at full depth reaches at its peak. Negative moves it the other way; 0 switches the route off.")],
    "DST3": [(None, "Strip route 3 destination", "STRIP page: what the mod strip moves on route 3. The same list as the LFOs' DEST (Appendix C); from the strip, LRAT and LDPT act on both LFOs. Func + turn goes from group to group.")],
    "AMT3": [(None, "Strip route 3 amount", "STRIP page: how far the strip, fully up, moves the destination of route 3, in that destination's own scale: +100 is what an LFO at full depth reaches at its peak. Negative moves it the other way; 0 switches the route off.")],
    "WVIB": [(None, "Strip vibrato", "STRIP page: vibrato the mod strip brings in, in semitones at the top of the strip. 0 = none.")],
    "DEST": [
        ("ARP/ACFG", "Pressure destination", "What pad pressure does to the arp: RATE, OCT or GATE."),
        ("AT ROUTE/", "Route destination", "What pad pressure moves on this track: OFF, what only pressure reaches (EXC, BOW, VIB), then everything an LFO of the engine can move, then the cells of the track's effects."),
        ("Synth/MENV", "Envelope destination", "AMP, CUT, PTCH or OFF, then every other destination the LFOs offer."),
        (None, "LFO destination", "What the LFO moves; the list depends on the engine (Appendix C)."),
    ],

    # ---- MIDI track ----------------------------------------------------------------------------
    "PROG": [(None, "Program", "Sends a program change.")],
    "VOL": [
        ("MIDI track/", "Volume (CC 7)", "Sends the volume controller."),
        (None, "Track volume", "Mixer volume of the track."),
    ],
    "MOD": [
        ("MIDI track/", "Modulation (CC 1)", "Sends the mod-wheel controller."),
        (None, "Reverb modulation", "Hall, spring and shimmer reverbs: slow sines move the reverb's delays. 0: a still tail, which rings more; up, the tail wavers and is chorused."),
    ],
    "EXPR": [(None, "Expression (CC 11)", "Sends the expression controller.")],
    "VRAT": [
        ("MIDI track/", "Vibrato rate (CC 76)", "Sends the vibrato-rate controller."),
        (None, "Strip vibrato rate", "STRIP page: the speed of the strip's vibrato (WVIB), 0.5–12 Hz."),
    ],
    "VDPT": [(None, "Vibrato depth (CC 77)", "Sends the vibrato-depth controller.")],
    "VDLY": [(None, "Vibrato delay (CC 78)", "Sends the vibrato-delay controller.")],
    "RVB": [(None, "Reverb send (CC 91)", "Sends the reverb-level controller.")],
    "CHOR": [(None, "Chorus send (CC 93)", "Sends the chorus-level controller.")],
    "CC#": [(None, "General controllers 16 to 23", "CC page: eight free controllers for whatever the external instrument maps them to.")],

    # ---- The TALK insert, and the eight reserved places at the end of every DEST list -------------------
    "BITE": [(None, "Bite", "Adds harmonics before the formants so quiet or pure sounds still talk.")],
    "CH.R": [(None, "Reserved destination", "A place kept in every DEST list for sounds saved before 22 November 2026 (the MODFX block's chorus rate). The block is gone, its effects are insert types: aimed here, a slot or an LFO moves nothing.")],
    "CH.D": [(None, "Reserved destination", "A place kept in every DEST list for sounds saved before 22 November 2026 (the MODFX block's chorus depth). The block is gone, its effects are insert types: aimed here, a slot or an LFO moves nothing.")],
    "FL.R": [(None, "Reserved destination", "A place kept in every DEST list for sounds saved before 22 November 2026 (the MODFX block's flanger rate). The block is gone, its effects are insert types: aimed here, a slot or an LFO moves nothing.")],
    "FL.D": [(None, "Reserved destination", "A place kept in every DEST list for sounds saved before 22 November 2026 (the MODFX block's flanger depth). The block is gone, its effects are insert types: aimed here, a slot or an LFO moves nothing.")],
    "PH.R": [(None, "Reserved destination", "A place kept in every DEST list for sounds saved before 22 November 2026 (the MODFX block's phaser rate). The block is gone, its effects are insert types: aimed here, a slot or an LFO moves nothing.")],
    "PH.D": [(None, "Reserved destination", "A place kept in every DEST list for sounds saved before 22 November 2026 (the MODFX block's phaser depth). The block is gone, its effects are insert types: aimed here, a slot or an LFO moves nothing.")],
    "RO.S": [(None, "Reserved destination", "A place kept in every DEST list for sounds saved before 22 November 2026 (the MODFX block's rotary speed). The block is gone, its effects are insert types: aimed here, a slot or an LFO moves nothing.")],
    "RO.D": [(None, "Reserved destination", "A place kept in every DEST list for sounds saved before 22 November 2026 (the MODFX block's rotary depth). The block is gone, its effects are insert types: aimed here, a slot or an LFO moves nothing.")],

    # ---- Insert effects ------------------------------------------------------------------------
    "VOIC": [(None, "Voices", "Number of chorus voices.")],
    "STER": [(None, "Stereo", "Offsets left and right so the tremolo moves across the stereo field.")],
    "TIME": [
        ('Wavetable/', 'Scan time', 'Time one pass through the table takes, while SYNC is off.'),
        ("ARP/", "Strum time", "Time from the first note of a strummed chord to its last (up to 1 second)."),
        ("Insert (CHR|ENS|FLG)/", "Centre delay", "The delay the sweep moves around, a few milliseconds: short whistles, long doubles. With THRU on the dry sound is delayed by this much, so the sweep passes through it."),
        (None, "Delay time", "Time between echoes. With SYNC on it picks a tempo division."),
    ],
    "DCAY": [
        ("ARP/ROLL", "Velocity decay", "How much each repeat is quieter than the last."),
        (None, "Decay time", "Length of the reverb tail."),
    ],
    "LOW": [(None, "Low gain", "Boost or cut of the low band.")],
    "MID": [(None, "Mid gain", "Boost or cut of the mid band.")],
    "MFRQ": [(None, "Mid frequency", "Centre frequency of the mid band.")],
    "HIGH": [(None, "High gain", "Boost or cut of the high band.")],
    "THR": [(None, "Threshold", "Level above which the compressor works.")],
    "MAKE": [(None, "Make-up gain", "Gain after compression.")],
    "CEIL": [(None, "Ceiling", "Highest level the limiter lets through.")],
    "LOOK": [(None, "Look-ahead", "How far ahead the limiter looks, in milliseconds.")],
    "WDTH": [(None, "Width", "Stereo width of the ensemble.")],
    "HORN": [(None, "Horn speed", "Fast-speed rotation of the treble horn.")],
    "DRUM": [(None, "Drum speed", "Fast-speed rotation of the bass rotor.")],
    "ACCL": [(None, "Acceleration", "How quickly the rotors change speed.")],
    "ASYM": [(None, "Asymmetry", "Works the chosen curve off-centre, on any curve: even harmonics (the octave) join the odd ones.")],
    "SAT": [
        ("Drums/KCK2", "Body saturation", "Kick machine: saturation of the body before the level envelope, 0–100: a rounder, louder, squarer body. (DRIV on the pad's page is the saturation after everything.)"),
        ("Insert AMP/", "Saturation curve", "The curve of the amp's two clipping stages: one of the eighteen saturation curves (Chapter 8). TANH to start with. On page FX2."),
        (None, "Loop saturation curve", "The curve the echo's repeats are bent by when DRIV is up: one of the eighteen saturation curves (Chapter 8). FAST to start with. On page FX2."),
    ],
    "FILT": [
        ("Insert WAH/", "Filter kind", "What the wah sweeps, 21 kinds: the state-variable, SEM, ladder, diode and comb filters in their modes (S.BP … CMB-), three phasers (PH2 PH4 PH6) and the vowel filter (VOWL)."),
        (None, "Smoothing filter", "Softens the reducer's aliasing."),
    ],
    "PDLY": [(None, "Pre-delay", "Gap before the reverb starts.")],
    "DISP": [(None, "Dispersion", "High sound travels round the loop faster than low: the spring's characteristic chirp (on SHIM a chirp in the shimmer's tail; 0 = none).")],
    "TENS": [(None, "Tension", "Spring tension: tighter and brighter, or looser and boingier.")],
    "SHIM": [(None, "Shimmer amount", "How much of the tail is fed back into it shifted by MODE's interval; 0 = none (and no cost). On every reverb: RVB, HALL, SPRG, SHIM.")],
    "HCUT": [(None, "High cut", "Darkens the reversed echoes.")],
    "OUT": [(None, "Output gain", "Output level in dB.")],
    "FREQ": [
        ("Insert WAH/", "Wah frequency", "Centre of the wah's sweep."),
        (None, "Carrier frequency", "Frequency the signal is multiplied with."),
    ],
    "SENS": [
        ('Sampler/', 'Sensitivity', 'How readily a hit is detected in TRNS mode; higher finds quieter hits.'),
        (None, "Sensitivity", "How far the playing level opens the wah (auto-wah)."),
    ],
    "GAIN": [(None, "Gain", "Amp preamp gain.")],
    "CAB": [(None, "Cabinet", "Cutoff of the speaker-cabinet filter.")],
    "BAND": [(None, "Band shift", "Vocoder: moves the bands this track is filtered with against the bands the input is measured in, by up to twelve bands either way.")],
    "LMID": [(None, "Low-mid gain", "Boost or cut of the low-mid band.")],
    "HMID": [(None, "High-mid gain", "Boost or cut of the high-mid band.")],
    "AGE": [(None, "Tape age", "Wow, flutter and dulling of the tape.")],
    "TAPS": [(None, "Taps", "Number of echoes in the pattern (MTAP: up to eight; RVSE: up to four reversed stretches).")],
    "SRC": [(None, "Sidechain source", "Which signal makes the track duck: SELF, another track T1 to T16, or IN (the track's input).")],
    "THRS": [
        ("ARP/ROL2", "Pressure threshold", "Pressure a held pad needs before it starts repeating."),
        (None, "Threshold", "Level of the source above which the track ducks."),
    ],
    "RELS": [(None, "Release", "How fast the track comes back after ducking.")],
}


def _first(key, *entries):
    """Put context entries in front of a label's existing ones (the first match wins)."""
    DESC[key] = list(entries) + DESC.get(key, [])


# ---- The modulation router's pages: XIN, SLT1 .., LSHP, STRIP, LFO1, LFO2. Every sound engine has them (the
# synth without LFO1 / LFO2: its LFOs are the screen's MOD pages) ---------------------------------------------
ROUTER = "[^/]+"
_first("SRC", ("[^/]+/SLT", "Slot source", "What the slot reads: OFF, LFO1 LFO2, ENV FENV (the amp and filter envelopes), VEL, PRES (this note's own pad pressure), STRP (the mod strip), KEY, RAND (one value a note), IN (the level of the track input), XIN XLP XHP XBP (the track input itself, sample by sample: as it is, low-passed, high-passed, both), VIB (the strip's vibrato), ONE (a constant), RND2 RND3 RND4 (three more values a note), ONST (a sound beginning in the input), BND1 to BN16 (the input's level in sixteen bands), then the engine's own (Chapter 7; the synth has LFO3 LFO4 and ENV2 to ENV5 after ONE). Func + turn jumps from group to group."))
_first("DEST", ("[^/]+/SLT", "Slot destination", "What the slot moves: the list the LFOs' DEST offers, then the modulation of modulation: L1RT L1DP L2RT L2DP (an LFO's rate, two octaves either way, and depth, up to double) and AMT1 to AM16 (another slot's amount); then, as every DEST list, the cells of the effects on the track (1:C1 … 8:WT) and eight reserved places (CH.R … RO.D) that move nothing."))
DESC.update({
    "AMT": [(None, "Slot amount", "How far the slot's source moves its destination, in the destination's own scale: +100 with the source at its top is where an LFO at full depth peaks. Negative moves it the other way; 0 switches the slot off. On pitch the amount counts squared: 10 is 12 cents, 50 three semitones, 100 an octave.")]})
DESC.update({
    "VIA": [(None, "Slot via", "A second source that scales the amount, taken as 0 to 100 % (one that swings both ways counts from its bottom): VIA PRES gives the slot only while the pad is pressed, VIA STRP lets the strip bring it in, VIA VEL makes it follow how hard the note was played. OFF: the amount as set.")]})
for _n in (1, 2):
    _first(f"DST{_n}", (ROUTER + "/XIN", f"Track input route {_n} destination", "XIN page: what the track input moves, sample by sample. The same list as the STRIP page's: on PTCH the input is frequency modulation, on CUT filter FM, on AMP amplitude or ring modulation, on a phase (PM, FM's PM1 to PMAL) phase modulation; on the synth RING and SYNC are destinations of their own. The input is the track chosen on TRK FX, page IN: the mean of its two sides."))
    _first(f"AMT{_n}", (ROUTER + "/XIN", f"Track input route {_n} amount", "XIN page: how far the input, at full level, moves route " + str(_n) + "'s destination, in that destination's own scale. A new amount is glided to over 4 ms."))
    DESC[f"FLT{_n}"] = [(None, f"Track input route {_n} filter", "XIN page: which form of the input route " + str(_n) + " reads: RAW as it is, LP through a low-pass at LPF, HP through a high-pass at HPF, BP through both (the same as a slot's sources XIN, XLP, XHP, XBP).")]
DESC.update({
    "LPF": [
    ("Wavetable/", "Input low-pass", "XIN page: the corner of the low-pass that FLT LP and BP, the sources XLP and XBP, and the wavetable's own XFLT and XRCT read the input through. On the wavetable it starts at 20 kHz (open), so XFLT and XRCT read the whole input until it is set; XFLT's filter is twice as steep as the router's."),
    (None, "Input low-pass", "XIN page: the corner of the low-pass that FLT LP and BP, and the slots' sources XLP and XBP, read the track input through (one pole, 6 dB an octave). Starts at 2 kHz."),
]})
DESC.update({
    "HPF": [
    ("Wavetable/", "Input high-pass", "XIN page: the corner of the high-pass that FLT HP and BP, the sources XHP and XBP, and the wavetable's own XFLT and XRCT read the input through. On the wavetable it starts at 20 Hz (open)."),
    (None, "Input high-pass", "XIN page: the corner of the high-pass that FLT HP and BP, and the slots' sources XHP and XBP, read the track input through (one pole, 6 dB an octave). Starts at 200 Hz."),
]})
for _n in range(1, 9):
    DESC[f"L{_n}"] = [(None, f"Custom LFO shape, point {_n}", "LSHP page: the level of the " + ["first", "second", "third", "fourth", "fifth", "sixth", "seventh", "eighth"][_n - 1] + " eighth of the track's own LFO shape, −100 to +100. An LFO whose SHPE is USR holds each level for an eighth of its cycle; USRS joins them with straight lines, the last back to the first.")]
_first("RATE", ("[^/]+/LFO[12]$", "LFO rate", "Speed in Hz. With SYNC on, DIV sets the speed from the tempo instead."))
_first("SHPE", ("[^/]+/LFO[12]$", "LFO shape", "SIN TRI SQR SAW, RND (stepped random), SRND (smooth random), RAMP, EXPU and EXPD (which only push one way), and USR / USRS: the track's own shape from the LSHP page, stepped or joined by lines."))
_first("SYNC", ("[^/]+/LFO[12]$", "LFO tempo sync", "On: the LFO's speed is DIV at the project's tempo."))
DESC.update({
    "TRIG": [
    ("Insert", "Effect LFO trigger", "What restarts the effect's LFO: FREE runs on; KEY, ONE, HALF and HOLD as on a sound's LFO, but restarted by a sound beginning in what the effect's KEY listens to (SELF: this track; IN: the track's input) instead of by a note."),
    (None, "LFO trigger mode", "FREE runs on; KEY restarts on each note; ONE plays one cycle then holds; HALF plays half a cycle; HOLD samples one value per note. On the LFO1 / LFO2 pages (and a drum pad's PLFO) four more: XKEY, XONE, XHLF and XHLD are KEY, ONE, HALF and HOLD restarted by a hit in the track input (a sound beginning there, as the source ONST hears it), in every voice at once; a note does not restart them. The synth's MOD page has the first five."),
]})
# ---- Per-pad modulation: PLFO and PSLT pages on the drum kit (every pad; U5) -------------------------
# The per-pad LFO reads the same sources and aims at the same destinations as the kit-wide LFOs
# but belongs to one pad. Each pad also has two free slots.
_first("RATE", ("Drums/PLFO", "Per-pad LFO rate", "Speed of this pad's own LFO, in Hz. The pad's LFO runs independently from the kit's two LFOs."))
_first("DPTH", ("Drums/PLFO", "Per-pad LFO depth", "How far this pad's own LFO moves its destination. Negative inverts."))
_first("SHPE", ("Drums/PLFO", "Per-pad LFO shape", "Same shapes as the kit's LFOs: SIN TRI SQR SAW RND SRND RAMP EXPU EXPD USR USRS."))
_first("DEST", ("Drums/PLFO", "Per-pad LFO destination", "What this pad's LFO moves, on this pad alone. The same list as the kit's LFOs: PTCH DEC TONE DRIV SNAP AMP PAN, the kick and resonator cells, PM WPOS ETIM FTIM, the track's effect cells."))
_first("TRIG", ("Drums/PLFO", "Per-pad LFO trigger", "FREE runs on; KEY restarts at each hit of this pad; ONE, HALF and HOLD as on the kit's LFOs (the list also shows XKEY to XHLD)."))
_first("SYNC", ("Drums/PLFO", "Per-pad LFO tempo sync", "On: the pad's LFO speed is DIV at the project's tempo."))
_first("DIV", ("Drums/PLFO", "Per-pad LFO division", "The tempo division of the pad's LFO while its SYNC is on."))
_first("FADE", ("Drums/PLFO", "Per-pad LFO fade-in", "Seconds from the trigger to full depth (0 = instant)."))
_first("SRC1", ("Drums/PSLT", "Pad slot 1 source", "PSLT page: what this pad's first slot reads. The same sources as the kit's slots (LFO1 and LFO2 are the kit's LFOs as this drum runs them): ENV, VEL, PRES, STRP, RAND, ONE and the rest."))
_first("DST1", ("Drums/PSLT", "Pad slot 1 destination", "What slot 1 moves. Same list as the kit's LFOs and the per-pad LFO: PTCH DEC TONE DRIV SNAP AMP PAN and the per-type cells."))
_first("AMT1", ("Drums/PSLT", "Pad slot 1 amount", "How far, and which way. 0 switches the slot off."))
_first("VIA1", ("Drums/PSLT", "Pad slot 1 via", "A second source that scales the amount: OFF for the amount as set."))
_first("SRC2", ("Drums/PSLT", "Pad slot 2 source", "What slot 2 reads."))
_first("DST2", ("Drums/PSLT", "Pad slot 2 destination", "What slot 2 moves."))
_first("AMT2", ("Drums/PSLT", "Pad slot 2 amount", "How far, and which way."))
_first("VIA2", ("Drums/PSLT", "Pad slot 2 via", "A second source that scales slot 2's amount."))

# ---- Wave sequencer, wavetable, FM: what the router's ports added or left unexplained ---------------------------
_first("LOOP", ("Wave seq/", "Loop mode", "How each lane goes round its steps: FWD, REV, PING (there and back), ONCE (then holds); RAND a step of the lane at random each time, drawn for each note (each voice its own walk, repeated by the same notes in the same order); ORND the random loop as it was before 22 November 2026, a short cycle that every voice of a chord shares, kept so that older sounds can be heard as they were."))
_first("PHAS", ("(Macro|Wavetable)/ANLG", "Start-phase spread", "Where in its cycle each oscillator starts: 0 always at the same place (the same attack every time), 100 anywhere."))
DESC.update({
    "WVAR": [(None, "Wave variation", "Each note reads the table a little elsewhere: up to half the table either way at 100.")]})
DESC.update({
    "PVAR": [(None, "Pan variation", "Each note sits up to this far left or right of the centre.")]})
DESC.update({
    "LVAR": [(None, "Level variation", "Each note is quieter by up to this much (100: down to silence); never louder.")]})
DESC.update({
    "SEED": [(None, "Random seed", "Which sequence of per-note draws, 0–127: the same seed and the same notes in the same order give the same sound every time. It repeats the ANLG page's spread, the sources RAND and RND2 to RND4 and the RAND scan together. Setting it starts the sequence again.")]})
for _n in range(1, 7):
    _first(f"WAV{_n}", ("FM/WAVE", f"Operator {_n} wave", f"What operator {_n} reads: SINE (the classic operator), CYCL (single cycles of the sample SMPL names, one period at each of 64 places along it) or one of the built-in wavetables. POS{_n} on the WPOS page is where in it."))
DESC.update({
    "POS1": [(None, "Operator 1 position", "WPOS page: where in its wave operator 1 reads (the table position, or which cycle of the sample). An LFO and slot destination, reached sample by sample.")],
    "POS2": [(None, "Operator 2 position", "WPOS page: where in its wave operator 2 reads (the table position, or which cycle of the sample). An LFO and slot destination, reached sample by sample.")],
    "POS3": [(None, "Operator 3 position", "WPOS page: where in its wave operator 3 reads (the table position, or which cycle of the sample). An LFO and slot destination, reached sample by sample.")],
    "POS4": [(None, "Operator 4 position", "WPOS page: where in its wave operator 4 reads (the table position, or which cycle of the sample). An LFO and slot destination, reached sample by sample.")],
    "POS5": [(None, "Operator 5 position", "WPOS page: where in its wave operator 5 reads (the table position, or which cycle of the sample). An LFO and slot destination, reached sample by sample.")],
    "POS6": [(None, "Operator 6 position", "WPOS page: where in its wave operator 6 reads (the table position, or which cycle of the sample). An LFO and slot destination, reached sample by sample.")],
})
_first("SMPL", ("FM/WAVE", "Cycle sample", "The sample whose single cycles the wave CYCL reads."))
DESC.update({
    "WSEQ": [(None, "Wave sequence", "FM, WAVE page. SCAN: an operator's position glides through its table. 2 to 16: every table becomes a sequence of that many of its waves, each held and then crossfaded into the next (XFAD); the position steps through them.")]})
_first("XFAD", ("FM/WPOS", "Sequence crossfade", "With WSEQ at 2 to 16: the share of each step spent crossfading into the next wave."))
DESC.update({
    "PENV": [(None, "Position envelope", "FM, WPOS page: each operator's own envelope moves its position, −100 to +100.")]})

# ---- Bodies: BODY / BDY2 (macro oscillator, wave sequencer), the resonator (RSN), the resonator drum (RSN) and
# the oscillator bank (BANK) ---------------------------------------------------------------------------------
BODIES = "STRG TUBE COMB PLAT BAR BELL HEAD GONG TAM CRSH RIDE CUP CHNA SPLS"
DESC.update({
    "MRPH": [
    ("Drums/", "Body offset", "Resonator drum: moves the body of the sound SND chose along the list " + BODIES + ", up to 13 places either way."),
    ("(Macro|Wave seq)/BODY", "Body", "BODY page: what the body is: " + BODIES + " (string, tube, comb, plate, bar, bell, drum head, gong, tam-tam and five cymbals), and anything between two neighbours."),
    (None, "Body", "Resonator: what the body is: " + BODIES + " (string, tube, comb, plate, bar, bell, drum head, gong, tam-tam and five cymbals), and anything between two neighbours."),
]})
DESC.update({
    "XMPH": [
    ("Drums/", "Exciter offset", "Resonator drum: moves the exciter of the sound SND chose along STRK BOW BRTH SCRP BRSH, up to 4 places either way."),
    (None, "Exciter", "What sets the body ringing: STRK (a strike), BOW, BRTH (breath), SCRP (a scrape), BRSH (a brush), and anything between."),
]})
DESC.update({
    "ENRG": [
    ("Drums/", "Energy offset", "Resonator drum: added to the sound's energy, −100 to +100."),
    (None, "Energy", "How hard the exciter drives the body: velocity times the envelope (or pad pressure, with a pressure route to EXC), times twice ENRG. A strike takes it once; a bow or breath follows it."),
]})
DESC.update({
    "VSEN": [(None, "Velocity sensitivity", "Resonator: how much a harder note drives the body harder. 0: every note alike.")]})
DESC.update({
    "STIF": [(None, "Stiffness", "Resonator, added to what MRPH gives: from a string's harmonic partials, through the stretched partials of a piano string, to those of a bar.")]})
DESC.update({
    "SHT": [(None, "Sheet", "Resonator, added to what MRPH gives: towards the dense, irregular partials of a plate.")]})
DESC.update({
    "SHEL": [(None, "Shell", "Resonator, added to what MRPH gives: towards a bell's partials (the minor third, the fifth, the octave above the strike note).")]})
DESC.update({
    "ODD": [(None, "Odd partials", "Resonator, added to what MRPH gives: the even partials leave, as in a tube closed at one end.")]})
DESC.update({
    "HUM": [(None, "Hum", "Resonator, added to what MRPH gives: the level of the hum, an octave under the note.")]})
DESC.update({
    "MEMB": [(None, "Membrane", "Resonator, BOD2: towards the partials of a round drum head (1, 1.59, 2.14, 2.30 …).")]})
DESC.update({
    "DROP": [(None, "Pitch drop", "Resonator, BOD2: the pitch rises with how hard the body moves and falls back as it dies, up to an octave: a tom's or a gong's glide.")]})
DESC.update({
    "WIRE": [
    ("Drums/", "Wires offset", "Resonator drum: added to the sound's WIRE, the rattle of something lying against the body."),
    (None, "Wires", "Resonator, BOD2: a noisy contact rattles with the body's motion, as loud as the body moves: snare wires on a head, the sizzle of a hat's two plates."),
]})
DESC.update({
    "MEET": [
    ("Drums/", "Meeting partial offset", "Resonator drum: added to the sound's MEET."),
    (None, "Meeting partial", "Resonator, BOD2: which partial a bow or breath meets on a heavy body, from the first towards the sixteenth (SPRD: how many around it)."),
]})
DESC.update({
    "PAIR": [(None, "Pairs", "Resonator, BOD2: every second partial comes to sit a few parts in a thousand above the one under it, so the two beat, each pair at its own rate.")]})
DESC.update({
    "HAND": [(None, "Hand-placed partials", "Resonator, BOD2: towards the partials placed by hand for the body MRPH stands at (the gong, the tam-tam and the cymbals have their own).")]})
DESC.update({
    "CONT": [(None, "Continuity", "Resonator, EXC page, added to what XMPH gives: from one burst to fed for as long as there is energy.")]})
DESC.update({
    "GRAN": [(None, "Grain", "Resonator, EXC page, added to what XMPH gives: what the excitation is made of, from one impulse through sparse irregular impulses to dense noise; with a sample on the GRAIN page the impulses are grains of it.")]})
DESC.update({
    "REED": [(None, "Reed", "Resonator, EXC page: the grip's curve, from a bow's (it holds while the difference is small and lets go when it grows) to a reed's (an opening the difference closes).")]})
DESC.update({
    "HARD": [(None, "Hardness", "Resonator, EXC page: dull to bright: the excitation's brightness, and how soon a bow lets go.")]})
_first("IN", ("Resonator/", "Track input", "Resonator, EXC page: how much the track input excites the body. Up to the middle the input is added at full beside the exciter; from there the exciter fades, and at the top the input alone drives the body. The input is gated by velocity and the envelope; pressure opens it up to twice."))
DESC.update({
    "LAG": [(None, "Energy lag", "Resonator, EXC page: how slowly the energy follows velocity, the envelope and pressure.")]})
_first("GPOS", ("Resonator/", "Grain position", "Resonator, GRAIN page: where in the sample the grains start."))
_first("SIZE", ("Resonator/GRAIN", "Grain size", "Resonator, GRAIN page: how long each grain is."))
DESC.update({
    "GPIT": [(None, "Grain pitch", "Resonator, GRAIN page: the grains transposed, up to two octaves either way.")]})
DESC.update({
    "COUP": [
    ("Drums/", "Products", "Oscillator bank: from the six squares' sum to their products in pairs (on RING the other way)."),
    ("(Macro|Wave seq)/BODY", "Body coupling", "BODY page: how the voice meets its body. 0: as a force, the body is struck by every sample of it and rings like a filter. Raised, the voice's level grips the body like a bow, and it sings by itself for as long as the voice sounds."),
    (None, "Coupling", "Resonator, EXC page: how much the exciter reacts to the body where it touches. None for a strike or a scrape; raised, it grips like a bow or a reed and the body sings by itself."),
]})
DESC.update({
    "EXC": [(None, "Excite", "Oscillator bank: how much the pad's pressure feeds the bands, which are held up for as long as it is pressed (above 1 the squares get the unrest of the resonator's breath and scrape).")]})
DESC.update({
    "SND": [(None, "Resonator sound", "Resonator drum: which of the resonator's ready sounds the pad plays. The other cells of the page move it from there (0 = the sound as it is).")]})

# ---- Insert effects' further cells (FX2) ---------------------------------------------------------------------
DESC.update({
    "THRU": [(None, "Through zero", "Flanger, chorus, ensemble, vibrato: on, the dry sound is delayed by TIME so the sweep passes through it; the two cancel completely at the crossing. The effect is then TIME late.")]})
DESC.update({
    "DLAY": [
    ("Insert VIB/", "Trigger delay", "The vibrato waits this long after a trigger (TRIG, KEY) before it fades in."),
    (None, "Trigger delay", "After a trigger (TRIG, KEY) the level or place is left alone this long, 0–2 s; then FADE brings the modulation in."),
]})
DESC.update({
    "READ": [(None, "Read mode", "How the echo is read: FWD as recorded; REV backwards, a stretch at a time; OCT+, 5TH+, OCT-, O+5 and SUB shift every repeat (an octave up, a fifth up, an octave down, an octave and a fifth up, two octaves down); HOLD shuts the loop, which repeats what it holds; STOP slows the tape to a standstill. A change fades the echo out and in.")]})
DESC.update({
    "LFRQ": [
    ("Insert GEQ/", "Low band frequency", "Where the lowest of the four bands sits."),
    (None, "Low shelf frequency", "Where the low shelf turns."),
]})
DESC.update({
    "LMFQ": [(None, "Low-mid band frequency", "Where the second band sits.")]})
DESC.update({
    "HMFQ": [(None, "High-mid band frequency", "Where the third band sits.")]})
DESC.update({
    "HFRQ": [
    ("Insert GEQ/", "High band frequency", "Where the highest of the four bands sits."),
    (None, "High shelf frequency", "Where the high shelf turns."),
]})
DESC.update({
    "MIDQ": [(None, "Mid width", "Width of the middle band: 0.1 is wide, 20 narrow.")]})
DESC.update({
    "KNEE": [(None, "Knee", "Width of the soft knee, 0–12 dB; 0 is a hard knee.")]})
DESC.update({
    "SCHP": [(None, "Sidechain high-pass", "A high-pass in front of the detector only (20 Hz = off): with it up, a bass drum or a low note no longer makes the effect pump.")]})
_first("XOVR", ("Insert ROT/", "Crossover", "Where the sound is split between the horn and the drum, 500 Hz–2 kHz."))
_first("WAVE", ("Insert RING/", "Carrier shape", "The waveform of the ring modulator's carrier (the LFO shapes)."))
DESC.update({
    "HEAD": [(None, "Playback heads", "One to four heads spaced evenly up to TIME. The last is the echo proper and feeds back; the earlier ones are a little quieter: a rhythmic figure instead of one repeat.")]})
DESC.update({
    "WIDE": [(None, "Width", "The right side's heads sit up to 20 ms later: a wider echo.")]})
DESC.update({
    "WOW": [(None, "Wow and flutter", "The tape transport's waver on all taps (wow at 0.6 Hz, flutter at 7 Hz); 0 is a steady line. At 100 the delay time wavers by about 0.4 %, so the pitch wavers more the longer the echo: some 30 cents at 0.5 s.")]})
_first("BODY", ("Insert VOC/", "Body", "Vocoder, TUNE mode: what the resonator the bands ring through is: " + BODIES + ", and between."))
DESC.update({
    "FRZ": [(None, "Freeze", "Vocoder: on, the bands hold the levels they have. In BAND the vowel stays; in TUNE the body goes on being driven with that spectrum.")]})

# ---- Cells whose general entry is another page's (build_manual.py: SUBJECT / FOREIGN) -----------------------
BODY_PAGES = "(Resonator/BODY|Macro/BODY|Wave seq/BODY)$"
_first("DCAY", ("(Resonator/RSN|Macro/BODY|Wave seq/BODY)$", "Body decay", "How long the body rings: the time its sound takes to fall by 60 dB, 0.05–20 s."))
_first("DAMP", (BODY_PAGES, "Body damping", "Added to the body's damping: how much sooner its high partials die than its low ones, −100 to +100 (up: wood; down: every partial rings as long, a comb filter)."),
       ("Drums/RSN", "Damping offset", "Resonator drum: added to the sound's DAMP, how much sooner the body's high partials die."))
_first("POS", (BODY_PAGES, "Body position", "Added to where the body is touched and driven, −100 to +100: from an end (every partial alike) towards the middle (the even partials are not reached)."))
_first("TILT", ("Resonator/BODY$", "Tilt", "Resonator, added to what MRPH gives: the level of the partials against their height; up, the high partials quieter."))
_first("SPRD", ("Resonator/BOD2$", "Meeting spread", "Resonator, BOD2: how many partials around MEET a bow or breath meets on a heavy body (0: that one alone; 100: a dozen each side)."))
_first("DIR", ("Resonator/BOD2$", "Direct sound", "Resonator, BOD2: the exciter itself heard beside the body: the stick's click, the brush's or the breath's hiss."),
       ("Drums/RSN", "Direct sound offset", "Resonator drum: added to the sound's DIR, the exciter heard beside the body."))
_first("CURV", ("Resonator/EXC$", "Grip curve", "Resonator, EXC page: the curve of a bow's grip and of a reed's opening: PHYS (the physical model's own) or one of the saturation curves."))
# ---- BDY2: exciter character, release behavior, body sounds (macro osc, wave sequencer) ----
BDY2_PAGES = "(Macro/BDY2|Wave seq/BDY2)$"
_first("CONT", (BDY2_PAGES, "Continuous", "BDY2: burst vs continuous. 0: one impulse at note-on, the body rings freely. 100: the voice feeds the body for as long as it sounds (the default)."))
_first("GRAN", (BDY2_PAGES, "Grain density", "BDY2: the resonator's own sparse impulses, from 2 a second up to noise at 100. 0 (default): only the voice excites the body."))
_first("REED", (BDY2_PAGES, "Friction curve", "BDY2: the junction's curve, from the bow's (0: holds while the difference is small, lets go when it grows) to the reed's (100: an opening the difference closes)."))
_first("HARD", (BDY2_PAGES, "Hardness", "BDY2: exciter brightness, offset from the auto value that scales with COUP. −100: duller, 0: as before, +100: brighter."))
_first("RDAM", (BDY2_PAGES, "Release damping", "BDY2: how quickly the body dies when the voice stops. 0 (default): ring on for the full DCAY. 100: killed at once (a palm mute)."))
_first("KDCY", (BDY2_PAGES, "Key decay", "BDY2: how much the decay follows the pitch. 0: no change. Positive: higher notes decay faster (halved per octave at 100). Negative: higher notes ring longer."))
_first("DIRE", (BDY2_PAGES, "Direct sound", "BDY2: the exciter itself heard beside the body: the click of a strike, the hiss of grains."))
_first("WIRE", (BDY2_PAGES, "Wire", "BDY2: a rattle against the body's motion: the snare wires on a head, the sizzle of two plates."))
_first("MIX", ("(Macro|Wave seq)/BODY$", "Body mix", "BODY page: how much of the voice is the body's sound. 0: off, the voice is not touched and the body is not run."))
_first("TUNE", ("(Macro|Wave seq)/BODY$", "Body tune", "BODY page: the body against the voice's note, ±24 semitones: a fifth, an octave, a beating unison."))
_first("KTRK", ("(Macro|Wave seq)/BODY$", "Body key tracking", "BODY page: how far the body follows the keys: 100 on the note, 0 a fixed body at middle C (plus TUNE) that every note is played through."))

# ---- The kick machine's second page, KCK2 (every cell at 0 is the kick machine as it was) -------------------
DESC.update({
    "KIND": [(None, "Knock kind", "Kick machine, KCK2: what the knock is. TICK: the short tick KNCK and KTON set. FM: the body is frequency-modulated instead, KNCK the depth (falling away with the sweep), KTON the modulator's ratio (half to eight times the pitch): the FM kick.")],
    "LIN": [(None, "Linear sweep", "Kick machine, KCK2: 0 the sweep falls evenly in pitch (the same number of semitones in every stretch of time); 100 evenly in Hz, which leaves the top fast and lingers low, as an analog kick does.")],
    "RES": [(None, "Tone resonance", "Kick machine, KCK2: resonance of the low-pass TONE sets, from none to a clear peak at its corner.")],
    "LPSW": [(None, "Tone sweep", "Kick machine, KCK2: the low-pass opens with the pitch sweep, up to 21 times its corner at the strike, and closes as the pitch falls: a bright front on a dark body.")],
    "LPKT": [(None, "Tone follows tune", "Kick machine, KCK2: 0 the low-pass is measured from 200 Hz; 100 from the kick's own pitch, so TONE means the same on a low and a high kick.")],
    "EDGE": [(None, "Edge", "Kick machine, KCK2: the front edge as SNAP makes it on the other drums. Below 0 the onset is softened; above 0 the first 3 ms are lifted and a noise click is added. 0: none.")],
    "PATH": [(None, "Stage order", "Kick machine, KCK2. KSYN: the kick machine's own order of stages. DRUM: the order of the older kick types (the low-pass on the body, then SAT, then the level envelope) and DRIV as the kit's common drive after everything: what FMKK, KICK and MKIK start with.")],
})
# ---- Each pad's oscillator wave: WAV1 (pads 1-8, the top row) and WAV2 (pads 9-16) -------------------------
DESC.update({
    "W#": [(None, "Pad wave", "WAV1 / WAV2 pages: the wave the oscillator of pad 1 to 16 reads (knob N of WAV1 is top pad N, of WAV2 bottom pad N): SINE, or one of the wavetable engine's built-in tables, read where the destination WPOS says. It is the body of a kick and of SUB; CLAP, SHKR, CYMB, the resonator drum and the oscillator bank have no such oscillator. (CYCL, which the other engines' wave lists have after SINE, is not offered: a drum has no sample whose cycles it could read. A kit saved with it played the sine and loads as SINE.)")],
})
_first("SMPL", ("Resonator/GRAIN", "Grain sample", "Resonator, GRAIN page: the sample GRAN's impulses are grains of. OFF: impulses of noise."))
_first("SCAN", ("Resonator/GRAIN", "Grain scan", "Resonator, GRAIN page: how fast the place the grains start at moves on through the sample, against the sample's own speed; 0 stays put, negative runs backwards."))
_first("SPRY", ("Resonator/GRAIN", "Grain spray", "Resonator, GRAIN page: each grain starts up to a quarter of a second either side of the place."))
_first("SHAP", ("Resonator/GRAIN", "Grain shape", "Resonator, GRAIN page: the window of each grain: HANN smooth, TRI, PERC percussive, SWEL swelling."))
_first("PJIT", ("Resonator/GRAIN", "Grain pitch jitter", "Resonator, GRAIN page: each grain up to an octave off its pitch."))

# Screens whose cells are built in UI code rather than by an engine.
# (short, long name, device, page, values, description)
EXTRA = [
    # PARAM, synth: the SRC page (cells of oscillator slots that are not analog waves; n = 1, 2 or 3)
    ("nPOS", "Wavetable position", "PARAM", "SRC", "0–100", "Synth, slot n set to WT: where in the table the oscillator reads."),
    ("nTBL", "Wavetable table", "PARAM", "SRC", "SAW … DIGI, VOWL … COMB", "Synth, slot n set to WT: which of the wavetable engine's 23 built-in tables (the first nine, then the fourteen newer ones)."),
    ("nIDX", "FM index", "PARAM", "SRC", "0–100", "Synth, slot n set to FM: how hard the modulator pushes the carrier."),
    ("nRAT", "FM ratio", "PARAM", "SRC", "1–12", "Synth, slot n set to FM: the modulator's pitch as a whole multiple of the note."),
    ("nFRM", "Formant frequency", "PARAM", "SRC", "0–100", "Synth, slot n set to FORM: the resonant peak, from 1 to 16 times the note's pitch."),
    ("nWID", "Formant width", "PARAM", "SRC", "0–100", "Synth, slot n set to FORM: how broad the peak is."),
    ("nTLT", "Harmonic tilt", "PARAM", "SRC", "0–100", "Synth, slot n set to HARM: from all harmonics equally loud (bright) to steeply falling (dark)."),
    ("nODD", "Odd harmonics", "PARAM", "SRC", "0–100", "Synth, slot n set to HARM: from all harmonics to odd ones only."),
    # TRACKS
    ("VOL", "Track volume", "TRACKS", "VOL", "0–100", "Mixer page: knob N is the volume of track N."),
    ("PAN", "Track pan", "TRACKS", "PAN", "L–R", "Mixer page: knob N is the pan of track N."),
    ("SND1", "Send 1", "TRACKS, TRK FX", "SND1 / TRACK", "0–100", "Level sent to bus 1 (reverb by default)."),
    ("SND2", "Send 2", "TRACKS, TRK FX", "SND2 / TRACK", "0–100", "Level sent to bus 2 (delay by default)."),
    ("ENG", "Engine", "TRACKS", "ENG", "9 engines", "Knob N chooses a pending engine for track N (VA FM MO WT WS SP DR MD RS); that track's top pad commits it."),
    # TRK FX
    ("BUS1", "Bus 1 effect", "TRK FX", "TRACK", "33 types", "The type of the first effect on send bus 1 (the whole chain is on S1FX). Shared by all tracks."),
    ("BUS2", "Bus 2 effect", "TRK FX", "TRACK", "33 types", "The type of the first effect on send bus 2 (the whole chain is on S2FX). Shared by all tracks."),
    ("LEN", "Track length", "TRK FX, STEP", "TRACK / bar", "1–1024", "Number of steps in this track's pattern; every track can differ (polymeter). STEP shows it as LN."),
    ("SPD", "Track speed", "TRK FX, STEP", "TRACK / bar", "1/4 1/2 1x 2x 4x", "Playback speed of the track against the tempo. STEP shows it as SP."),
    ("GATE", "Arp gate lock", "TRK FX", "ARPLK", "0–100", "Per-step lock of the arpeggiator's gate length."),
    ("VEL", "Arp velocity lock", "TRK FX", "ARPLK", "0–100", "Per-step lock of the arpeggiator's velocity."),
    ("RTCH", "Arp ratchet lock", "TRK FX", "ARPLK", "1–4", "Per-step lock of the arpeggiator's ratchet count."),
    ("SLOT", "Insert slot", "TRK FX", "FX, FX2, S1FX .. S2FX2", "slot number", "Which of the track's insert slots the page shows; on a send bus's pages, which of the effects on that bus."),
    ("TYPE", "Insert effect type", "TRK FX", "FX, FX2, S1FX .. S2FX2", "OFF, 33 types", "Effect in the selected slot. Changing it replaces the effect and its values (an undo step); OFF takes the effect out."),
    ("ON", "Insert on", "TRK FX", "FX, S1FX, S2FX", "OFF / ON", "Switches the slot on or off. Effects with a TIME or RATE show SYNC here instead."),
    ("WET", "Insert wet level", "TRK FX", "FX, S1FX, S2FX", "0–100", "How much of the effect is heard; 0 takes it out. An effect put on a send starts at 100."),
    ("SRC", "Input source", "TRK FX", "IN", "OFF, T1 …", "The track this track listens to: its input. Never the track itself. No step lock and no pressure range."),
    ("LEVL", "Input level", "TRK FX", "IN", "0–100", "How loud the input is taken, for everything that listens to it."),
    ("MODE", "Input mode", "TRK FX", "IN", "OFF MIX RING", "OFF: the input is only listened to (by the vocoder, keyed effects, the XIN page, the slots). MIX: it is also added to this track's sound. RING: this track's sound is multiplied by it."),
    ("FATK", "Follower attack", "TRK FX", "IN", "ms", "How fast the level of the input (the source IN) rises."),
    ("FREL", "Follower release", "TRK FX", "IN", "ms", "How fast it falls."),
    # STEP
    ("LN", "Track length", "STEP", "bar", "1–1024", "Length of the selected track in steps. Made longer, the track loops through the new steps."),
    ("SP", "Track speed", "STEP", "bar", "1/4 … 4x", "Speed of the selected track."),
    ("QN", "Playback quantize", "STEP", "bar", "0–100", "Pulls recorded notes toward the grid on playback; the recording itself is never changed."),
    ("NOTE", "Step note", "STEP", "TRIG", "C-1 … G9", "Pitch of the step; a chord moves together."),
    ("VEL", "Step velocity", "STEP", "TRIG", "0–127", "Velocity of the step's notes."),
    ("LEN", "Note length", "STEP", "TRIG", "0.25–16 steps", "How long the step's note is held."),
    ("MICR", "Micro-timing", "STEP", "TRIG", "−50…+50 %", "Moves the step earlier or later by up to half a step."),
    ("COND", "Trig condition", "STEP", "TRIG", "ALL PROB A:B FILL !FIL 1ST !1ST PRE !PRE", "When the step plays (Appendix C)."),
    ("PROB", "Probability", "STEP", "TRIG", "0–100 %", "Chance the step plays, with COND = PROB."),
    ("A", "A of A:B", "STEP", "TRIG", "1–8", "With COND = A:B the step plays on pass A of every B passes."),
    ("B", "B of A:B", "STEP", "TRIG", "1–8", "The cycle length for A:B."),
    ("LCKS", "Locks", "STEP", "TRIG", "count", "Number of parameter locks on the step; turning it anticlockwise clears them (an undo step)."),
    ("PRES", "Recorded pressure amount", "STEP", "PRES", "0–200 %", "How much of the pad pressure recorded with the step's notes is played back. 0 plays none and keeps the recording; hold > and turn for 100."),
    (">LCK", "Pressure to locks", "STEP", "PRES", "four clicks", "Turns the step's recorded pressure into parameter locks on the steps the note spans, through the track's pressure routes as they are now, then removes the recording."),
    ("CLR", "Clear pressure", "STEP", "PRES", "four clicks", "Removes the pad pressure recorded with the step's notes. The notes stay."),
    # DRUMS bar
    ("TN", "Tune", "DRUMS", "bar", "20 Hz–12 kHz", "TUNE of the selected drum pad."),
    ("DC", "Decay", "DRUMS", "bar", "0.01–4 s", "DEC of the selected drum pad."),
    ("TO", "Tone", "DRUMS", "bar", "0–100", "TONE of the selected drum pad."),
    ("DV", "Drive", "DRUMS", "bar", "0–100", "DRIV of the selected drum pad."),
    ("SN", "Snap", "DRUMS", "bar", "0–100", "SNAP of the selected drum pad."),
    ("LV", "Level", "DRUMS", "bar", "0–100", "LEVL of the selected drum pad."),
    ("PN", "Pan", "DRUMS", "bar", "L–R", "PAN of the selected drum pad."),
    ("TY", "Type", "DRUMS", "bar", "10 types", "TYPE of the selected drum pad."),
    ("SW ST SC SH KN KT HD AC", "Kick page", "DRUMS", "bar, page KICK (a kick pad)", "as KICK", "SWEP SWPT SCRV SHAP KNCK KTON HOLD ACRV of the selected kick pad."),
    ("KD LN RS LS LK SA ED PA", "Kick page 2", "DRUMS", "bar, page KCK2 (a kick pad)", "as KCK2", "KIND LIN RES LPSW LPKT SAT EDGE PATH of the selected kick pad."),
    ("SN MR XM EN DP WR MT DR", "Resonator drum page", "DRUMS", "bar, page RSN (a resonator drum)", "as RSN", "SND MRPH XMPH ENRG DAMP WIRE MEET DIR of the selected resonator drum."),
    ("MD PG CL EX DP BL CP LF", "Bank page", "DRUMS", "bar, page BANK (an oscillator bank)", "as BANK", "MODL PING COLR EXC DAMP BLOM COUP LOFI of the selected oscillator bank."),
    ("RT DP SH DS TG SY DV FD", "Pad LFO page", "DRUMS", "bar, page PLFO (every pad)", "as PLFO", "RATE DPTH SHPE DEST TRIG SYNC DIV FADE of the selected pad's own LFO. RT, DP and FD say SET DEST while DS is OFF."),
    ("S1 D1 A1 V1 S2 D2 A2 V2", "Pad slots page", "DRUMS", "bar, page PSLT (every pad)", "as PSLT", "SRC1 DST1 AMT1 VIA1 SRC2 DST2 AMT2 VIA2 of the selected pad's two own slots. A1 / A2 say SET DST1 / SET DST2 while their destination is OFF."),
    # PRESET bar
    ("PS", "Preset", "PRESET", "bar", "list", "Scrolls the preset list."),
    ("CT", "Category", "PRESET", "bar", "ALL INIT BASS PAD LEAD KEYS FX PERC KIT MULT", "Filters the list by category. MULT lists the multi presets (several connected tracks at once) and nothing else."),
    ("EN", "Engine filter", "PRESET", "bar", "THIS / ANY", "Show presets for this track's engine only, or all."),
    ("AU", "Audition", "PRESET", "bar", "OFF / ON", "On (the default): the highlighted preset takes over the track's sound for listening only; the previous sound returns on leaving unless LD is used. Off: the list scrolls without changing the sound."),
    ("LD", "Load", "PRESET", "bar", "four clicks", "Keeps the highlighted preset as the track's sound for good. Under MULT: loads every member of the multi into the earliest empty tracks (one undo step)."),
    ("SV", "Save", "PRESET", "bar", "four clicks", "Saves the track's sound as a user preset (User 1, User 2, ...); the Mac then asks for a name. A track that listens to another track, or is listened to, is saved with those tracks as one multi preset."),
    ("DL", "Delete", "PRESET, SONG", "bar", "four clicks", "Deletes the user preset (PRESET) or the selected row (SONG)."),
    ("VL", "Volume", "PRESET", "bar", "0–100", "Track volume, to level-match while browsing."),
    # SETTINGS
    ("BPM", "Tempo", "SETTINGS", "SETUP", "20–300", "Tempo. Shift = 0.1 steps, Func = steps of 10."),
    ("SWNG", "Swing", "SETTINGS, ARP", "SETUP / ARP / ROL2", "0–100", "Delays every other step. SETUP: the pattern's swing. ARP and ROL2: the arp's and the roll's own."),
    ("QNT", "Quantize", "SETTINGS", "SETUP", "0–100", "Global playback quantize."),
    ("CLK", "Clock source", "SETTINGS", "SETUP", "INT EXT AUTO LINK", "MIDI clock: internal, external, or automatic. LINK is listed but not available yet."),
    ("MVOL", "Master volume", "SETTINGS", "SETUP", "0–100", "Overall output level."),
    ("PAT", "Pattern", "SETTINGS", "SETUP", "1–16", "Selects the pattern (same as > + pad)."),
    ("HELP", "Help cards", "SETTINGS", "SETUP", "OFF 0.35S INST", "When holding Shift or Func alone shows its help card: never, after 0.35 seconds (the default), or at once. Kept between sessions."),
    ("LOW", "Master EQ low", "SETTINGS", "MASTR", "±18 dB", "Low band of the master EQ."),
    ("MID", "Master EQ mid", "SETTINGS", "MASTR", "±18 dB", "Mid band of the master EQ."),
    ("MFRQ", "Master EQ mid frequency", "SETTINGS", "MASTR", "100 Hz–8 kHz", "Centre of the mid band."),
    ("HIGH", "Master EQ high", "SETTINGS", "MASTR", "±18 dB", "High band of the master EQ."),
    ("THR", "Master compressor threshold", "SETTINGS", "MASTR", "−60–0 dB", "Level above which the master compressor works."),
    ("RATI", "Master compressor ratio", "SETTINGS", "MASTR", "1–20", "Strength of the master compression."),
    ("MAKE", "Master make-up gain", "SETTINGS", "MASTR", "0–30 dB", "Gain after the master compressor."),
    ("CEIL", "Master limiter ceiling", "SETTINGS", "MASTR", "−3–0 dB", "Highest level the master limiter lets through."),
    ("SLOT", "Project slot", "SETTINGS", "PROJ", "P01–P32", "Which project slot LOAD, SAVE and SVAS act on."),
    ("LOAD", "Load project", "SETTINGS", "PROJ", "four clicks", "Loads the slot. Shows OK or FAIL."),
    ("SAVE", "Save project", "SETTINGS", "PROJ", "four clicks", "Saves to the current slot."),
    ("SVAS", "Save as", "SETTINGS", "PROJ", "four clicks", "Saves to the slot chosen with SLOT; the Mac then asks for a name (Cancel takes the save back)."),
    ("NEW", "New project", "SETTINGS", "PROJ", "four clicks", "Starts an empty project."),
    ("CLCK", "Metronome click", "SETTINGS", "CLICK", "OFF REC PLAY", "When the metronome clicks: never, while recording runs (the default), or whenever the sequencer plays. One click a beat, the first beat of each bar higher and louder. Kept between sessions, not in the project."),
    ("CLVL", "Click level", "SETTINGS", "CLICK", "0–100", "Loudness of the click. Every turn plays one click, so it can be set with the sequencer stopped. The click is added after the master effects and MVOL: they do not change it, and it is never recorded into a resample."),
    ("CNT", "Count-in", "SETTINGS", "CLICK", "OFF 1BAR 2BAR", "Bars the metronome counts in before a recording starts. The count-in always clicks, also with CLCK off. Nothing plays or is recorded until it ends."),
    # ARP
    ("MODE", "Arp mode", "ARP", "ARP", "15 modes", "Order the held notes play in (Appendix C)."),
    ("RATE", "Arp rate", "ARP", "ARP", "1/1–1/64T", "Note value of the arp's steps."),
    ("OCT", "Octave range", "ARP", "ARP", "1–8", "Octaves the arp spans."),
    ("GATE", "Gate", "ARP", "ARP / ROLL", "0–100", "Length of each arp note (or roll repeat) as a share of its step."),
    ("RPT", "Repeats", "ARP", "ARP", "1–8", "Times each note is repeated before moving on."),
    ("LTCH", "Latch", "ARP", "ARP / ROL2", "OFF / ON", "ARP: the arp keeps playing after the keys are released (also Shift + Arp). ROL2: a pad press starts a roll that runs until pressed again; this one is off after every start and every project load."),
    ("STRM", "Strum", "ARP", "PLAY", "OFF / ON", "Plays held notes as a strummed chord."),
    ("TIME", "Strum time", "ARP", "PLAY", "0–1000 ms", "Time from the first note of a strummed chord to its last. A note whose turn has not come when the step ends is not played: a long strum wants a slow RATE."),
    ("DIR", "Strum direction", "ARP", "PLAY", "UP DOWN ALT", "Direction of the strum."),
    ("VEL", "Strum velocity", "ARP", "PLAY", "0–100", "Velocity change across the strum."),
    ("RTCH", "Ratchet", "ARP", "PLAY", "1–4", "Repeats inside each arp step."),
    ("RDEC", "Ratchet decay", "ARP", "PLAY", "0–100", "How much each ratchet repeat is quieter."),
    ("HUMN", "Humanize", "ARP", "PLAY / ROL2", "0–100", "Random variation of timing and velocity."),
    ("FREZ", "Freeze", "ARP", "PLAY", "OFF / ON", "Holds the current note order while you change keys."),
    ("SPCE", "Spice", "ARP", "VARY / MUTA", "0–100", "Variation macro: how adventurous the variations are."),
    ("DICE", "Dice", "ARP", "VARY", "0–100", "Re-rolls the random choices behind the variations."),
    ("DENS", "Density", "ARP", "VARY", "0–100", "How many steps sound."),
    ("SCAT", "Octave scatter", "ARP", "VARY", "0–100", "Chance of notes jumping octave."),
    ("RCHN", "Ratchet chance", "ARP", "VARY", "0–100", "Chance of a step ratcheting."),
    ("GVAR", "Gate variation", "ARP", "VARY", "0–100", "Spread of gate lengths."),
    ("STRP", "Strip target", "ARP", "VARY", "OFF SPCE DICE DENS SCAT RCHN GVAR GATE", "Which macro the mod strip drives while this track's arp is on."),
    ("VAR", "Variation", "ARP", "VARY", "number", "Selects a stored variation."),
    ("MUTA", "Mutate", "ARP", "MUTA", "OFF / ON", "Switches mutation on: the arp pattern changes by itself over time. SPCE on the same page sets how much."),
    ("INTV", "Mutate interval", "ARP", "MUTA", "1–16", "Bars between mutations."),
    ("EVOL", "Evolution", "ARP", "MUTA", "0–99", "Generations mutated so far; anticlockwise returns to the ancestor (an undo step)."),
    ("ANC", "Ancestor", "ARP", "MUTA", "ORIG / KEPT", "What mutations return to: the original pattern or the last kept one."),
    (">PAT", "Write to pattern", "ARP", "MUTA", "four clicks", "Writes the arp's current steps into the track's pattern (an undo step)."),
    ("DEST", "Pressure destination", "ARP", "ACFG", "RATE OCT GATE", "What pad pressure does to the arp."),
    ("DPTH", "Pressure depth", "ARP", "ACFG / ROLL", "0–100", "How far pressure moves it."),
    ("REC", "Record mode", "ARP", "ACFG", "NOTE / CHORD", "How arp output is written when recording."),
    ("LOCK", "Macro / lock interaction", "ARP", "ACFG", "ADD MACRO LOCK MULT", "How variation macros and step locks combine."),
    ("TAP", "Tap rhythm", "ARP", "ACFG", "OFF / ON", "While on, every pad press taps a step of a custom rhythm."),
    ("RHYT", "Rhythm", "ARP", "ACFG", "OFF / ON", "Plays the arp in the tapped rhythm."),
    ("ROLL", "Roll", "ARP", "ROLL", "OFF / ON", "Hold-to-ratchet for every pad that plays a note."),
    ("BASE", "Base rate", "ARP", "ROLL", "division or Hz", "Repeat rate at the lightest pressure (1/8 to begin with)."),
    ("MAX", "Maximum rate", "ARP", "ROLL", "division or Hz", "Repeat rate at full pressure (1/128 to begin with). Pressing harder steps through every rate from BASE to MAX."),
    ("P>V", "Pressure to velocity", "ARP", "ROLL", "0–100", "How much pressure sets each repeat's velocity."),
    ("ACNT", "Accent", "ARP", "ROL2", "every Nth", "Accents every Nth repeat."),
    ("PROB", "Repeat probability", "ARP", "ROL2", "0–100", "Chance each repeat sounds."),
    ("RPTS", "Repeat limit", "ARP", "ROL2", "0–16", "Stops after this many repeats; 0 = no limit."),
    # SCALE bar
    ("PG", "Scale page", "SCALE", "bar", "12 factory + user", "Which pad layout (scale) the pads play."),
    ("RT", "Root", "SCALE", "bar", "C–B", "Root note of the scale."),
    ("MD", "Mode", "SCALE", "bar", "RL / AB", "Relative (pads follow the root) or absolute."),
    ("ED", "Edit", "SCALE", "bar", "OFF / ON", "Edit mode: tap a pad, then a key assigns its note."),
    ("KS", "Keys select pages", "SCALE", "bar", "OFF / latched / momentary", "Keyboard keys choose the scale page."),
    ("KM", "Key mapping", "SCALE", "bar", "root / number", "Keys pick a page by its root or by its number."),
    ("OC", "Octave", "SCALE", "bar", "octaves", "Octave of the pads."),
    ("LK", "Scale lock", "SCALE", "bar", "OFF / ON", "Forces keys and arps into the scale (also the Scale button)."),
    ("NT", "Note", "SCALE", "bar (edit)", "note", "Edit mode: the selected pad's note."),
    ("PD", "Pad", "SCALE", "bar (edit)", "1–16", "Edit mode: which pad is being edited."),
    # CHORD bar
    ("BK", "Chord bank", "CHORD", "bar", "1–4", "Which bank of sixteen chord pads (also Up / Down)."),
    ("TR", "Transpose", "CHORD", "bar", "−24 to +24", "Moves every chord pad by semitones; what is stored stays as played."),
    ("OC", "Octave", "CHORD", "bar", "−3 to +3", "Moves every chord pad by octaves."),
    ("VL", "Velocity", "CHORD", "bar", "PAD / BALANCE / AS KEYS", "PAD: every note at the pad's velocity. BALANCE: the loudest note at the pad's velocity, the others below it as the keys were played. AS KEYS: every note exactly as hard as its key was played, however the pad is hit."),
    ("CL", "Clear bank", "CHORD", "bar", "four clicks", "Empties the sixteen pads of this bank (an undo step)."),
    ("TM", "Strum time", "CHORD", "bar (STRUM)", "0–1000 ms", "From a chord's first note to its last; 0 = together. Shift for single milliseconds."),
    ("DR", "Strum direction", "CHORD", "bar (STRUM)", "UP / DOWN / ALTERNATE", "Lowest note first, highest first, or one stroke each way."),
    ("HU", "Humanize", "CHORD", "bar (STRUM)", "0–100 %", "Uneven timing: each stroke up to 30 % quicker or slower, its notes unequally spaced."),
    ("VR", "Vary", "CHORD", "bar (STRUM)", "0–100 %", "A changing pattern: a stroke the other way now and then, neighbouring notes changing places. Every note still plays."),
    ("FL", "Fall", "CHORD", "bar (STRUM)", "0–100 %", "Later notes of a stroke are softer."),
    ("SL", "Chord-set slot", "CHORD", "bar (SETS), knob 1", "1–32", "Which chord-set slot to load from or save to (files in ~/Music/Neptune/Chords/); the name bar names it (SETS C1), and its readout marks a slot that holds a set with a *."),
    ("LD", "Load chord set", "CHORD", "bar (SETS), knob 3", "four clicks", "Loads the chord set from the selected slot into the four banks (SLOT EMPTY when there is none)."),
    ("SV", "Save chord set", "CHORD", "bar (SETS), knob 8", "four clicks", "Saves the four banks of chords to the selected slot."),
    # PERF grid
    ("TRK", "Target", "PERF", "FX", "SEL / MSTR / T1 …", "What the shown layer acts on: the track selected at the press, the whole mix, or one track."),
    ("FX", "Effect", "PERF", "FX", "--, DUB FRZ RPT STOP, then the insert effects", "The shown layer's effect. Choosing one gives the layer that effect's starting values (an undo step); -- empties the layer."),
    ("MODE", "Punch or throw", "PERF", "FX", "PNCH / THRW", "PNCH: the effect replaces the sound while the pad is down. THRW: the sound is sent to the effect while the pad is down and the result is added, ringing on afterwards."),
    ("WET", "Effect level", "PERF", "FX, PRESS, PRES", "0–100; as an amount −100 to +100", "FX: how much of the effect is heard. PRESS / PRES: how far the press, or full pressure, moves that level."),
    ("TIME", "Echo time / stop time", "PERF", "FX (DUB, STOP)", "seconds, or a division with SYNC", "DUB: the time between repeats. STOP: how long the tape takes to stop."),
    ("FDBK", "Feedback", "PERF", "FX (DUB)", "0–100", "How much of each repeat goes round again; the top of the range feeds on itself."),
    ("TONE", "Loop tone", "PERF", "FX (DUB)", "0–100", "Where the band filter in the echo's loop sits, low to high: every repeat is narrowed towards it."),
    ("DRIV", "Loop drive", "PERF", "FX (DUB)", "0–100", "How hard the repeats hit the tape: more squash and harmonics, more treble lost on every repeat, a less steady pitch."),
    ("SAT", "Tape curve", "PERF", "MORE (DUB)", "the eighteen saturation curves", "The curve the dub echo's tape saturates with (Chapter 8). FAST to start with."),
    ("HEAD", "Playback heads", "PERF", "MORE (DUB)", "1–4", "The dub echo's heads, spaced evenly up to TIME: the last is the echo proper and feeds back, the earlier ones a rhythmic figure."),
    ("WIDE", "Width", "PERF", "MORE (DUB)", "0–100", "The right side's heads sit up to 20 ms later: a wider echo."),
    ("READ", "Read mode", "PERF", "MORE (DUB)", "FWD REV OCT+ 5TH+ OCT- O+5 SUB HOLD STOP", "How the dub echo's tape is read: as recorded, backwards, shifted, held or slowed to a stop."),
    ("GRAB", "Grab time", "PERF", "FX (FRZ)", "0.02–2 s", "How long the freeze listens after the press before it shuts its input."),
    ("SHIM", "Shimmer", "PERF", "FX (FRZ)", "0–100", "Octave-up feedback while the pad is held: the frozen sound climbs."),
    ("SPRL", "Spiral", "PERF", "FX (FRZ)", "0–100", "Octave-up feedback after the release: the sound spirals away upward as it dies."),
    ("DEC", "Decay", "PERF", "FX (FRZ)", "0.2–20 s", "How long the frozen sound takes to die away once the pad is let go."),
    ("DIV", "Slice / division", "PERF", "FX (RPT), MORE", "note values", "RPT: the length of the repeated slice, 1/4 to 1/64. On MORE: the division SYNC locks the layer's time or rate to."),
    ("DCAY", "Repeat decay", "PERF", "FX (RPT)", "0–100", "Every repeat quieter than the last."),
    ("PTCH", "Repeat pitch", "PERF", "FX (RPT)", "−12 to +12 st", "Every repeat this many semitones from the last."),
    ("GATE", "Slice gate", "PERF", "FX (RPT)", "10–100", "How much of each slice sounds."),
    ("CURV", "Curve", "PERF", "FX (STOP), PRES", "0–100; LIN / SOFT / HARD", "STOP: the way the tape slows. On PRES: whether the pad's pressure answers evenly, early or late."),
    ("LTCH", "Latch", "PERF", "PRESS", "OFF / ON", "A press switches the pad on, the next one off."),
    ("QNT", "Press quantize", "PERF", "PRESS", "OFF 1/16 1/8 1/4 BAR", "While the sequencer plays, a press takes hold on the next of these."),
    ("LY", "Layer", "PERF", "PRESS", "A B C D", "Which of the pad's four layers the knobs show."),
    ("(value)", "Press amount", "PERF", "PRESS", "−100 to +100", "Under each value's own knob: how far a press moves that value for as long as the pad is down."),
    ("(value)", "Pressure amount", "PERF", "PRES", "−100 to +100", "Under each value's own knob: how far full pressure moves that value. Adds to the press amount."),
    ("SYNC", "Tempo sync", "PERF", "MORE", "OFF / ON", "Locks the layer's time or rate to the tempo (effects that have one)."),
    ("TAKE", "Clear take", "PERF", "MORE", "four clicks", "Clears the recorded pad moves in this pattern (an undo step). NO TAKE when nothing has been recorded."),
    ("CLR", "Clear pad", "PERF", "MORE", "four clicks", "Empties the pad, all its layers (an undo step)."),
    # SONG bar
    ("PA", "Pattern", "SONG", "bar", "1–16", "Pattern this row plays."),
    ("RP", "Repeats", "SONG", "bar", "count", "Times the row repeats."),
    ("TR", "Transpose", "SONG", "bar", "semitones", "Transposes pitched tracks for this row."),
    ("RW", "Row", "SONG", "bar", "row", "Selects the row (with fewer than two rows there is nothing to select, and the name bar says so)."),
    ("LP", "Loop", "SONG", "bar", "OFF / ON", "Loops the song at the end."),
    ("SG", "Song on", "SONG", "bar", "OFF / ON", "Arms song mode: Play then follows the rows."),
    ("IN", "Insert row", "SONG", "bar", "turn", "Inserts a row."),
    # AT ROUTE
    ("DEST", "Route destination", "AT ROUTE", "page 1 (DEST), top row", "per engine", "What pad pressure moves, for routes 1 to 4 of this track (a drum kit: one route). In this order: OFF; what only pressure reaches (EXC, BOW, VIB); everything the engine's LFOs can move; the cells of the track's effects."),
    ("DPTH", "Route depth", "AT ROUTE", "page 1 (DEST), bottom row", "−100…+100", "How far, in the destination's own scale; negative lowers. A drum kit's own destinations 0 to 100. ROUTE OFF while the route has no destination; one newly chosen starts at 50."),
    ("BIP", "Bipolar", "AT ROUTE", "page 2 (BIP), top row", "OFF / ON", "The pad read both ways: half pressure is the middle, less lowers, more raises. KIT ONE WAY on a drum's own destinations."),
    # WAVE
    ("ST", "Start", "WAVE", "WAVE", "position", "Start marker (the sampler's STRT)."),
    ("EN", "End", "WAVE", "WAVE", "position", "End marker (END)."),
    ("LS", "Loop start", "WAVE", "WAVE", "position", "Loop start marker (LPST)."),
    ("LE", "Loop end", "WAVE", "WAVE", "position", "Loop end marker (LPEN)."),
    ("ZM", "Zoom", "WAVE", "WAVE", "factor", "Zooms the waveform view."),
    ("SC", "Scroll", "WAVE", "WAVE", "position", "Scrolls the zoomed view (zoom in with ZM first)."),
    ("MD", "Play mode", "WAVE", "WAVE", "FWD … GRAN", "The sampler's MODE."),
    ("SL", "Slices", "WAVE", "WAVE", "1–64", "Number of slices (SLCS)."),
    ("KY", "Key frame", "WAVE", "WAVE (wavetable track)", "1–8", "Which key frame of the KEYS table the page shows and edits. The top-row pads choose it too."),
    ("SM", "Key sample", "WAVE", "WAVE (wavetable track)", "OFF, list", "The sample this key is taken from (SMPn). Giving a key a sample selects the KEYS table."),
    ("PO", "Key position", "WAVE", "WAVE (wavetable track)", "position", "Where in the sample the key's cycle is taken (POSn): the line on the waveform."),
    ("PL", "Key place", "WAVE", "WAVE (wavetable track)", "0–100", "Where along the table the key sits (PLCn)."),
    ("AL", "Algorithm", "WAVE", "WAVE (wavetable track)", "LIN … VOC", "How the frames between two keys are made (ALGO)."),
    ("TB", "Table", "WAVE", "WAVE (wavetable track)", "list", "The track's table (TABL)."),
    ("SR", "Source", "WAVE", "REC", "MSTR, T1 …", "What is recorded: the master output or one track after its effects."),
    ("TH", "Threshold", "WAVE", "REC", "level", "Recording starts when the source passes this level."),
    ("LN", "Length", "WAVE", "REC", "free, 1 beat … 8 bars", "Length of the take."),
    ("QN", "Start quantize", "WAVE", "REC", "NOW / BAR", "Start at once, or on the next bar."),
    ("RC", "Record", "WAVE", "REC", "arm / stop", "Two clicks clockwise within a second arm; one anticlockwise stops."),
    ("NM", "Normalise", "WAVE", "REC", "OFF / ON", "Raises the take to full level."),
    ("SM", "Sample select", "WAVE", "REC", "list", "Chooses the track's sample, or the USER wavetable's source."),
    ("CR", "Crop", "WAVE", "REC", "four clicks", "Trims the sample to its start and end markers."),
    # MOD pages (main synth)
    ("LFO", "LFO select", "Synth", "MOD / MOD2", "LFO1–LFO4", "Which LFO the page edits."),
    ("LFOS", "LFO count", "Synth", "MOD", "1–4", "How many LFOs each voice runs."),
    ("DLY", "LFO delay", "Synth", "MOD2", "seconds", "Wait after note-on before the LFO starts."),
    ("ENVS", "Envelope count", "Synth", "MENV", "0–4", "How many extra envelopes each voice runs."),
]

# LFO destinations (the DEST cell of every LFO page) that are not a page cell under its own name.
# A destination that IS a page cell moves that cell: its entry above says what the cell does.
DESTS = {
    "OFF": ("No destination", "all engines", "The LFO does nothing and costs nothing."),
    "PTCH": ("Pitch", "all engines", "Pitch of the whole voice, up to an octave either way; the lower half of DPTH is vibrato territory."),
    "AMP": ("Level", "all engines", "The voice's level, from silence to double: tremolo."),
    "CUTB": ("Filter B cutoff", "Synth", "The cutoff of filter B alone, up to four octaves either way."),
    "PIT#": ("Oscillator 2 pitch", "Synth", "PIT2: the pitch of oscillator 2 alone, up to an octave either way."),
    "LVL#": ("Oscillator level", "Synth", "LVL1 to LVL3: the level of one oscillator, from silence to double."),
    "RAT#": ("LFO rate", "Synth", "RAT1 to RAT4: the rate of LFO 1 to 4, up to two octaves either way."),
    "DPT#": ("LFO depth", "Synth", "DPT1 to DPT4: the depth of LFO 1 to 4, from nothing to double."),
    "#A": ("Oscillator source, first cell", "Synth", "1A, 2A, 3A: the first SRC cell of oscillator 1, 2 or 3 (table position, FM index, formant frequency or harmonic tilt, by what the slot plays)."),
    "#B": ("Oscillator source, second cell", "Synth", "1B, 2B, 3B: the second SRC cell of oscillator 1, 2 or 3 (table, FM ratio, formant width or odd / even balance); tables and ratios change in steps."),
    "FATK": ("Filter envelope attack", "Synth, FM, Macro, Wavetable, Wave seq", "The ATK cell of the F ENV page."),
    "FDEC": ("Filter envelope decay", "Synth, FM, Macro, Wavetable, Wave seq", "The DEC cell of the F ENV page."),
    "FSUS": ("Filter envelope sustain", "Synth, FM, Macro, Wavetable, Wave seq", "The SUS cell of the F ENV page."),
    "FREL": ("Filter envelope release", "Synth, FM, Macro, Wavetable, Wave seq", "The REL cell of the F ENV page."),
    "FVEL": ("Velocity to cutoff", "Synth, FM, Macro, Wavetable, Wave seq", "The VEL cell of the F ENV page."),
    "FMOR": ("Filter morph", "Macro", "The MORP cell of the FILTR page (the macro oscillator's own MORP is its fourth macro)."),
    "WPOS": ("Wave position", "Drums", "Where in its WAVE (a table) a drum's oscillator reads, 0 to 1: a kick or a tom that moves through a table as it sounds. On the sine it does nothing."),
    "PM": ("Phase", "Synth, Wavetable, Wave seq, Drums", "The oscillator's phase (drums: the drum's own oscillator, a cycle either way). Synth: oscillator 1's, on the cross-modulation path (what the old XIN cell's PM did). Wavetable: up to two cycles either way, the amount squared; wave sequencer: one cycle either way. With the track input as the source (the XIN page, or a slot from XIN, or on the wavetable XFLT or XRCT) it is phase modulation: the other track's sound becomes sidebands around every harmonic of the note. From an LFO it is a vibrato of its own kind; a source that stands still is not heard."),
    "ATK#": ("Envelope attack", "Synth, FM", "FM, ATK1 to ATK6: the attack of operator 1 to 6. Synth, ATK2 to ATK5: the attack of an extra envelope (MENV)."),
    "DEC#": ("Envelope decay", "Synth, FM", "FM, DEC1 to DEC6: the decay of operator 1 to 6. Synth, DEC2 to DEC5: the decay of an extra envelope (MENV)."),
    "SUS#": ("Envelope sustain", "Synth, FM", "FM, SUS1 to SUS6: the sustain of operator 1 to 6. Synth, SUS2 to SUS5: the sustain of an extra envelope (MENV)."),
    "REL#": ("Envelope release", "Synth, FM", "FM, REL1 to REL6: the release of operator 1 to 6. Synth, REL2 to REL5: the release of an extra envelope (MENV)."),
    "EDPT": ("Extra envelope depth", "Synth", "The DPTH cell of the MENV page."),
    "DET#": ("Operator detune", "FM", "DET1 to DET6: the DETN cell of operator 1 to 6."),
    "FDB#": ("Operator feedback", "FM", "FDB1 to FDB6: the FDBK cell of operator 1 to 6."),
    "LRAT": ("Other LFO's rate", "FM, Macro, Wavetable, Wave seq, Sampler, Drums, RSN", "The rate of the other LFO, up to two octaves either way."),
    "LDPT": ("Other LFO's depth", "FM, Macro, Wavetable, Wave seq, Sampler, Drums, RSN", "The depth of the other LFO, from nothing to double."),
    "ETIM": ("Amp envelope time", "Synth, FM, Macro, Wavetable, Wave seq, Sampler, RSN, Drums", "How long the amp envelope takes, every stage of it (and of the envelope chain): up to eight times as long or an eighth at full depth, without changing ATK, DEC and REL. FM: every operator's envelope. Sampler: its envelope's attack, decay and release. Set under a held note, the running stage goes on faster or slower from where it is. With ANLG's EVAR (wavetable) the note's own variation is kept on top."),
    "FTIM": ("Filter envelope time", "Synth, FM, Macro, Wavetable, Wave seq, RSN, Drums", "How long the filter envelope takes, every stage of it: up to eight times as long or an eighth, without changing the F ENV page's cells."),
    "PM#": ("Operator phase", "FM", "PM1 to PM6: added to the phase of operator 1 to 6, one radian at full depth (the index of a modulator at full level). From XIN the track input is a seventh modulator; from OP1 to OP6 it is a connection between operators that no algorithm has, heard one sub-block (16 samples at 48 kHz) late."),
    "PMAL": ("Every operator's phase", "FM", "As PM1 to PM6, on all six operators at once."),
    "RING": ("Ring", "Synth", "The mixer's output (the three oscillators and the noise, before DRIV) times a gain of 1 + the sum. A slot that brings the track input here (the XIN page, or a slot from XIN, XLP, XHP or XBP) rings: at amount d the mixer becomes (1 - d) + d x the input, from the voice alone to the ring of the two; without an input the voice is left alone. From an LFO it is a tremolo before the filter."),
    "SYNC": ("Oscillator sync", "Synth", "Oscillator 1 (and its stack) starts its cycle again each time what is sent here rises through a small level after having been below it the other way. From the track input (the XIN page) the oscillator follows the input's period: the amount is the sensitivity. From an LFO it restarts once a cycle."),
    "GPOS": ("Grain position", "Sampler", "GRAN mode: where new grains start, around the scan position."),
    "SSIZ": ("Stretch size", "Sampler", "The SIZE cell of the STRCH page (SIZE alone is the grain size)."),
    "SSPR": ("Stretch spread", "Sampler", "The SPRD cell of the STRCH page (SPRD alone is the grain spread)."),
    "BMIX": ("Body mix", "Macro, Wave seq", "The MIX cell of the BODY page: how much of the voice is the body's sound."),
    "BMRP": ("Body", "Macro, Wave seq", "The MRPH cell of the BODY page: which body, moved along its list."),
    "BDCY": ("Body decay", "Macro, Wave seq", "The DCAY cell of the BODY page: how long the body rings."),
    "BCUP": ("Body coupling", "Macro, Wave seq", "The COUP cell of the BODY page: from a body struck by the voice to one it bows."),
    "BDMP": ("Body damping", "Macro, Wave seq", "The DAMP cell of the BODY page: how much sooner the body's high partials die."),
    "BPOS": ("Body position", "Macro, Wave seq", "The POS cell of the BODY page: where the body is driven."),
    "BTUN": ("Body tune", "Macro, Wave seq", "The TUNE cell of the BODY page: the body against the note."),
    "BKTK": ("Body key tracking", "Macro, Wave seq", "The KTRK cell of the BODY page: how far the body follows the keys."),
    "BCNT": ("Body continuity", "Macro, Wave seq", "The CONT cell of the BDY2 page: from one burst at the note to fed for as long as the voice sounds."),
    "BGRN": ("Body grain", "Macro, Wave seq", "The GRAN cell of the BDY2 page: the body's own sparse impulses, up to noise."),
    "BRED": ("Body friction curve", "Macro, Wave seq", "The REED cell of the BDY2 page: from a bow's grip to a reed's."),
    "BHRD": ("Body hardness", "Macro, Wave seq", "The HARD cell of the BDY2 page: how bright what drives the body is."),
    "BRDM": ("Body release damping", "Macro, Wave seq", "The RDAM cell of the BDY2 page: how quickly the body dies when the voice stops."),
    "BKDC": ("Body key decay", "Macro, Wave seq", "The KDCY cell of the BDY2 page: how much the decay follows the pitch."),
    "BDIR": ("Body direct sound", "Macro, Wave seq", "The DIRE cell of the BDY2 page: the exciter heard beside the body."),
    "BWIR": ("Body wire", "Macro, Wave seq", "The WIRE cell of the BDY2 page: a rattle against the body's motion."),
    "GSIZ": ("Grain size", "RSN", "The SIZE cell of the resonator's GRAIN page."),
    "#:C#": ("Insert cell", "Synth, FM, Macro, Wavetable, Wave seq, Sampler, RSN, Drums", "1:C1 to 8:C8: a cell of the effect in one of the track's eight insert slots; the first number is the slot, C1 to C4 are the four cells of its FX page, C5 to C8 those of FX2. The display names the cell as the effect now in the slot has it (1TIME, 2:MIX), or n:-- where there is nothing to move: an empty slot, a list or a count, an effect that takes no modulation (DRV, WSH, RED, AMP). Full depth moves the cell by its whole range from where the knob stands, a time or a frequency by ratio; the effect glides to it and the knob reads as set. The value is the track's, not a voice's: the note played last decides it, and with no note sounding the LFO goes on, so a free LFO moves a delay time in silence."),
    "#:WT": ("Insert wet level", "Synth, FM, Macro, Wavetable, Wave seq, Sampler, RSN, Drums", "1:WT to 8:WT: the WET of an insert slot (shown as 1:WET), from all dry to all effect."),
}

EXTRA += [(short.replace("#", "n"), long_name, engines, "LFO DEST", "destination", text)
          for short, (long_name, engines, text) in DESTS.items()]
