# Neptune Groovebox

## Session Recovery (READ THIS FIRST)
At the start of a session or after compaction:
1. Read `.ralph/ralph_task.md` — the anchor: success criteria, checkboxes, "Current Focus". Defines "done."
2. Read `.ralph/progress.md` — current state, how to run things, known issues, next steps, decisions.
3. Read `.ralph/guardrails.md` — learned constraints. Follow every sign.
4. Do NOT re-read the full codebase unless progress.md says something is broken. Trust the files. What a control
   does: `docs/launchkey-mk4-control-map.md` → "CONTROL GRAMMAR"; screens, pages, cells: `tools/manual/`.
   Firmware work: `docs/launchkey-mk4-firmware-map.md` section 0 and `docs/launchkey-mk4-firmware-rules.md` first.
5. Pick up "Current Focus" (ralph_task.md) and "Next Steps" (progress.md).
6. Before exiting or if context feels heavy: update progress.md (current state, not a log) and what's next.

## Compaction Instructions
When compacting this conversation, preserve:
- Current task and its completion state
- Any new guardrails discovered this session
- Any new known issues
- The exact next step to take
Do NOT preserve: file contents already read, full API/command outputs, failed approaches
(a failure worth remembering becomes a sign in `.ralph/guardrails.md`).

## Project Purpose
A macOS standalone application (C++ with JUCE) that turns a Novation Launchkey Mini 25 MK4 into an eyes-free, Elektron-style groovebox with deep synthesis and sampling. The Launchkey is the only interface required — the Mac screen is optional. Design not yet built, and why: `.ralph/spec.md`.

## Architecture

Most classes are header-only; the only `.cpp` files are `main.cpp`, `DisplayRenderer`, `LaunchkeyController`,
`PadManager`, `VoiceAllocator`, `SequencerCore` and `ScreenManager`. Exact list: `find src -type f`.

```
src/
├── main.cpp                — entry point; builds and wires everything (screens, Shift+pad table, arps, projects)
├── controller/
│   ├── LaunchkeyController — MIDI/SysEx with the Launchkey, feature controls, encoder translation
│   ├── ControllerProfile.h — abstraction layer for future controllers
│   ├── PadManager          — pad modes and change-only pad LED refresh
│   ├── DisplayRenderer     — 128x64 1-bit rendering, ack-paced SysEx frames
│   ├── CustomScaleManager.h — custom scale pad pages
│   ├── KeyboardSplit.h     — Shift + Oct key split, through the firmware's debug command
│   └── BitmapBenchmark.h   — frame-rate benchmark (never run on hardware)
├── sequencer/
│   ├── SequencerCore       — sample-accurate clock, pattern bank, trig conditions, recording, pressure lanes, mute/solo
│   ├── Pattern.h, Track.h, Step.h — data model (below)
│   ├── SongMode.h, SongPlayer.h — ordered list of patterns; the listener that moves the song on as track 1 wraps
│   └── PadRatchet.h        — hold-to-ratchet pads
├── engine/
│   ├── EngineBase.h, ParamSpec.h — engine interface; parameter specs, pages and shared id ranges
│   ├── AudioEngine.h       — audio callback, track mixer, parameter entry point, step locks, effects, sends, pads/arps
│   ├── VoiceAllocator      — polyphonic voice management
│   ├── UnifiedVoice.h, UnifiedVoiceEngine.h, ArchitecturePreset.h — configurable voice architecture
│   ├── FMEngine.h, FMVoice.h, FMOperator.h — 6-operator FM
│   ├── MacroOscEngine.h    — macro oscillator
│   ├── WavetableEngine.h   — wavetable with vintage modes
│   ├── WaveSeqEngine.h     — wave sequencing
│   ├── SamplerEngine.h, SampleBank.h — sampler (with granular mode) and its samples
│   ├── Resampler.h         — records a track or the master into a new sample
│   ├── DrumEngine.h, DrumVoice.h — drum kit
│   ├── MIDITrack.h         — external MIDI output track
│   ├── VoiceFilterSection.h, EngineLfoSection.h — per-voice filter; two LFOs, their destination lists and the
│   │                         STRIP page, shared by the non-unified engines
│   ├── AftertouchRouter.h  — pad-pressure routes, one set per track
│   ├── ModStripFollower.h  — every track keeps the value the mod strip last gave it
│   └── SendBusManager.h — not used by the app yet
├── dsp/
│   ├── filters/            — SVF, SEM, ladder, diode, comb; FilterSlot.h offers them by type and mode
│   ├── effects/            — EffectBase.h, EffectChain.h, TrackFx.h (DIRT / TALK / MODFX blocks), FxBlocks.h (shared
│   │                         building blocks), one header per effect
│   └── Envelope.h, EnvelopeChain.h, LFO.h, ModMatrix.h, DSPUtils.h
├── arpeggiator/            — Arpeggiator.h (one per track), VariationMacros.h, MutateSystem.h, RhythmCapture.h
├── ui/
│   ├── ScreenManager       — `Screen` base class; the control grammar (modifiers, hold layers, copy / clear / paste,
│   │                         undo / redo, help cards, pad colours), name bar, shared visual standard
│   ├── GrammarHooks.h      — binds ScreenManager's buttons to the AudioEngine (arps, track sound, tempo)
│   ├── ParamGrid.h, GridScreen.h — the 4x2 cell grid and a declarative grid-screen base
│   ├── FilterCurve.h       — the filter picture, measured from the real filters
│   ├── EnvelopeCurve.h     — the envelope picture, drawn from the real Envelope class
│   ├── ScreenAudit.h       — turns every knob on every page, holds every help key, checks the display
│   ├── TurnNotice.h        — a knob with nothing to act on says why in the name bar
│   ├── UiClock.h           — the screens' clock (the audit can move it on)
│   ├── screens/            — Home (TRACKS), StepView, ParamPage (PARAM, and `ParamCellScreen`, the base it shares
│   │                         with TrackFx and FilterView), TrackFx (TRK FX), DrumPad, Preset, Settings, FilterView
│   │                         (FLTR), Arp, CustomScale, Song, Aftertouch (AT ROUTE), Waveform (WAVE);
│   │                         Mixer and SubParamOverlay are unused
│   ├── ScreenLayout.h, BitmapFont.h, Icons.h, NoteUtils.h, ParamConstants.h, PadLEDFeedback.h, StartupAnimation.h
│   └── OLEDMirrorComponent.h, VirtualControllerComponent.h, MainContentComponent.h — the Mac window
├── project/
│   ├── Project.h, ProjectManager.h, ProjectBridge.h — project files; capture / apply of the whole live state
│   ├── Preset.h, PresetManager.h, FactoryPresets.h — sound presets
│   ├── UndoHistory.h       — 15 levels of undo / redo; ScreenManager fills and applies it
│   └── DrumKit.h, KitPreset.h, SamplePool.h — KitPreset, SamplePool not used by the app yet
└── sync/                   — MIDIClock.h (clock out; no clock input is wired); AbletonLink.h (wrapper, no library)
```

Tests: `tests/main_test.cpp` plus `tests/*_tests.h` (JUCE unit tests, one console binary). Firmware patches:
`tools/build_neptune_fw.py` + `neptune-patches/`. Owner's manual: `tools/manual/` → `docs/Neptune-Groovebox-Manual.pdf`.

## Key Interfaces

### Controller → App (MIDI In) — Neptune firmware NPE; full map in `docs/launchkey-mk4-control-map.md`
- DAW port — pads: Note On/Off channel 1 (notes 96–103 top row, 112–119 bottom row), velocity + polyphonic aftertouch (status A0h; needs feature 0x6C = 2)
- DAW port — encoders: 8 endless rotaries, relative, CC 0x55–0x5C channel 16, value 64 ± steps (needs feature 0x45 = 127); the app translates them to its internal CC 0x15–0x1C step format
- DAW port — buttons: Shift CC 63 ch7; Settings CC 108, Arp CC 109, Scale CC 110 on ch1 (127 press / 0 release); Func CC 105; button right of top pad row CC 104; Up/Down CC 106/107 (CC 102/103 with Shift); encoder-bank up/down CC 51/52; Play CC 115; Record CC 117
- MIDI port — keys: Note On/Off (notes 48–72 at octave 0), velocity, on the channel set by feature 0x64 (the app sets channel 1); pitch strip: pitch bend; mod strip: CC 1
- Octave up/down is handled inside the firmware and sends nothing
- SysEx: ready-for-next-frame acknowledgement
- Clock and transport messages from the controller's ports are ignored

### App → Controller (MIDI Out / SysEx)
- Pad LED colors: per-pad RGB via SysEx
- OLED display: 1,216-byte bitmap SysEx (19 bytes × 64 rows, 7 pixels per byte) to the stationary target (the firmware never draws over it)
- Feature controls (CC on channel 7, DAW port): 0x45 relative encoders, 0x64 keys channel, 0x6C pad aftertouch, 0x6E = 0 device MIDI clock output off, 0x1D pad layout, 0x1E encoder layout
- SysEx: DAW mode negotiation

### Step Data Model (`src/sequencer/Step.h`)
```
Note {
  pitch: int
  velocity: float
  lane: PressureLane      // pad pressure recorded with the note; empty = none
}
Step {
  trig: bool
  notes: Note[]           // polyphonic
  velocity: float
  length: float           // in steps
  microTiming: float      // -0.5 to +0.5 steps
  trigCondition: TrigConditionData
  paramLocks: Map<ParamId, float>
  pressureScale: float    // PRES: how much of the notes' recorded pressure plays, 0–2
  pageChangeLock: int     // custom scale page, -1 = none; stored, not applied yet
}
```

### Track Data Model (`src/sequencer/Track.h`)
```
Track {
  engineType: EngineType
  soundParams: Map<ParamId, float>  // engine parameters AND mixer, sends, effects, arp locks, pressure ranges
                                    // (id ranges: .ralph/progress.md → Code map → Parameters)
  steps: Step[]
  length: int             // independent per track, 1–1024
  speedMultiplier: float
  midiChannel: int        // for MIDI tracks
  muted: bool
  soloed: bool
}
```
Arpeggiator settings live in the per-track `Arpeggiator` objects, insert chains and send buses in `AudioEngine`;
`ProjectBridge` saves them with the project.

## Environment
- **Language:** C++17
- **Framework:** JUCE 9.0.5, fetched by CMake (standalone app target, later AU/VST3)
- **Platform:** macOS (Apple Silicon + Intel)
- **Controller:** Novation Launchkey Mini 25 MK4, USB-C, bus powered, running Neptune firmware NPE
- **Build:** `cmake --build build` is Debug, for the tests. The app runs from the optimised build: `cmake --build build-release` → `build-release/NeptuneGroovebox_artefacts/Release/Neptune Groovebox.app`
- **Test commands:** `cmake --build build --target NeptuneTests && (cd build && ctest)` (binary `build/NeptuneTests_artefacts/Debug/NeptuneTests`)
- **Everything else** (lag pass, manual build, screen dump, bench): `.ralph/progress.md` → How to run
- **Screen audit:** `python3 tools/screen_audit.py [--photos]` (device connected, app not running) → `.ralph/screen-audit/report.txt`
- **Reference docs:** Novation MK4 Programmer's Reference Guide (`.ralph/launchkey_mk4_programmers_reference.pdf`: Novation's, git-ignored; download it from Novation)

## Design Principles
1. **Eyes-free first.** Every performance function reachable from the Launchkey alone. The Mac screen is never required.
2. **Clean-room.** Elektron, Korg and Arturia workflows inspire the design. No proprietary code, assets or trademarks.
3. **Configurable, not fixed.** Track counts, pattern lengths, LFO counts, effect slots, send buses — no arbitrary caps. Limits come from CPU and memory.
4. **Controller-first settings.** Every configurable setting adjustable from the Launchkey OLED/knobs, not only from the Mac.
5. **Consistent gestures.** Hold-plus-turn and Shift-combos instead of menus. The same gesture means the same thing everywhere.
6. **Per-voice modulation.** Polyphonic aftertouch from the pads must reach individual voices, not be averaged.
7. **Sample-accurate sequencer.** The clock is independent of audio engines and can drive internal tracks and external MIDI.
8. **Ack-paced display.** Send a new OLED bitmap only after the Launchkey's ready reply. Never queue frames. Latest frame wins. Static screens not resent. Only finished pictures are sent; screen state is changed and drawn under `ScreenManager::getLock()`.
9. **Licensing awareness.** The project is AGPL-3.0. Check the licence of anything before linking it; Airwindows (MIT) preferred for effects.
10. **Hardware abstraction.** A controller-profile layer so other controllers can be added later without changing the core.
11. **Firmware is fair game.** The Launchkey MK4 firmware may be analyzed and modified to reclaim intercepted gestures and extend the controller's capabilities. The hardware was purchased specifically for this level of customization.
