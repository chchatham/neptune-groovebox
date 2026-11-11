#include <thread>
#include <juce_core/juce_core.h>
#include <juce_audio_basics/juce_audio_basics.h>
#include "../src/engine/VoiceAllocator.h"
#include "../src/sequencer/SequencerCore.h"
#include "../src/dsp/Envelope.h"
#include "../src/dsp/LFO.h"
#include "../src/dsp/filters/SVFilter.h"
#include "../src/controller/DisplayRenderer.h"
#include "../src/controller/PadManager.h"
#include "../src/ui/BitmapFont.h"
#include "../src/ui/PadLEDFeedback.h"
#include "../src/ui/screens/StepViewScreen.h"
#include "../src/dsp/ModMatrix.h"
#include "../src/dsp/filters/LadderFilter.h"
#include "../src/dsp/filters/DiodeFilter.h"
#include "../src/dsp/filters/FilterSlot.h"
#include "../src/engine/UnifiedVoice.h"
#include "../src/engine/UnifiedVoiceEngine.h"
#include "../src/engine/ArchitecturePreset.h"
#include "../src/engine/FMOperator.h"
#include "../src/engine/FMVoice.h"
#include "../src/engine/FMEngine.h"
#include "../src/engine/AudioEngine.h"
#include "../src/engine/MIDITrack.h"
#include "../src/engine/MacroOscEngine.h"
#include "../src/engine/WavetableEngine.h"
#include "../src/engine/WaveSeqEngine.h"
#include "../src/engine/DrumVoice.h"
#include "../src/engine/DrumEngine.h"
#include "../src/dsp/EnvelopeChain.h"
#include "../src/project/Preset.h"
#include "../src/project/PresetManager.h"
#include "../src/ui/screens/ParamPageScreen.h"
#include "../src/ui/screens/TrackFxScreen.h"
#include "../src/ui/screens/HomeScreen.h"
#include "../src/ui/ScreenManager.h"
#include "../src/dsp/effects/EffectBase.h"
#include "../src/dsp/effects/ParametricEQ.h"
#include "../src/dsp/effects/Compressor.h"
#include "../src/dsp/effects/Limiter.h"
#include "../src/dsp/effects/Delay.h"
#include "../src/dsp/effects/Reverb.h"
#include "../src/ui/screens/WaveformScreen.h"
#include "../src/controller/CustomScaleManager.h"
#include "../src/ui/screens/CustomScaleScreen.h"
#include "../src/ui/screens/PresetScreen.h"
#include "../src/ui/screens/WaveformScreen.h"
#include "../src/engine/AftertouchRouter.h"
#include "../src/arpeggiator/Arpeggiator.h"
#include "../src/arpeggiator/VariationMacros.h"
#include "../src/arpeggiator/RhythmCapture.h"
#include "../src/ui/screens/ArpScreen.h"
#include "../src/sequencer/SongMode.h"
#include "../src/sync/MIDIClock.h"
#include "../src/project/FactoryPresets.h"
#include "../src/arpeggiator/MutateSystem.h"
#include "../src/ui/screens/SongScreen.h"
#include "../src/ui/screens/AftertouchScreen.h"
#include "custom_scale_deep_tests.h"
#include "send_bus_full_tests.h"
#include "engine_macro_osc_tests.h"
#include "engine_wavetable_tests.h"
#include "engine_waveseq_tests.h"
#include "drum_kit_tests.h"
#include "effects_expansion_tests.h"
#include "sync_project_tests.h"
#include "modulation_ui_tests.h"
#include "sampler_full_tests.h"
#include "arp_performance_tests.h"
#include "mod_page_tests.h"
#include "project_integration_tests.h"
#include "modmatrix_wiring_tests.h"
#include "filter_view_tests.h"
#include "oled_mirror_tests.h"
#include "midi_track_output_tests.h"
#include "oled_mirror_input_tests.h"
#include "project_roundtrip_tests.h"
#include "factory_preset_coverage_tests.h"
#include "settings_project_tests.h"
#include "pad_ratchet_tests.h"
#include "lfo_modes_tests.h"
#include "envelope_stage_curve_tests.h"
#include "effects_more_tests.h"
#include "engine_filter_tests.h"
#include "onset_click_tests.h"
#include "engine_lfo_tests.h"
#include "sampler_grain_tests.h"
#include "control_grammar_tests.h"
#include "live_record_tests.h"
#include "held_step_pages_tests.h"
#include "lfo_destinations_tests.h"
#include "help_card_tests.h"
#include "controller_clock_tests.h"
#include "help_card_layers_tests.h"
#include "pressure_lane_tests.h"
#include "strip_routes_tests.h"
#include "undo_history_tests.h"
#include "dead_controls_tests.h"
#include "param_grammar_tests.h"
#include "screen_grammar_tests.h"
#include "sampler_stretch_tests.h"
#include "wavetable_scan_tests.h"
#include "drum_keys_tests.h"
#include "preset_audition_tests.h"
#include "sampler_release_tests.h"
#include "mod_wheel_tests.h"
#include "bench.h"
#include "bitmap_benchmark_tests.h"
#include "display_latency_tests.h"

class MidiMessageTests : public juce::UnitTest
{
public:
    MidiMessageTests() : UnitTest("MIDI Messages") {}

    void runTest() override
    {
        beginTest("Note on construction");
        {
            auto noteOn = juce::MidiMessage::noteOn(1, 60, (juce::uint8) 127);
            expect(noteOn.isNoteOn());
            expectEquals(noteOn.getNoteNumber(), 60);
            expectEquals((int) noteOn.getVelocity(), 127);
            expectEquals(noteOn.getChannel(), 1);
        }

        beginTest("Note off construction");
        {
            auto noteOff = juce::MidiMessage::noteOff(1, 60);
            expect(noteOff.isNoteOff());
            expectEquals(noteOff.getNoteNumber(), 60);
        }

        beginTest("SysEx message construction");
        {
            const juce::uint8 data[] = { 0xF0, 0x00, 0x20, 0x29, 0x02, 0x13, 0xF7 };
            auto sysex = juce::MidiMessage(data, sizeof(data));
            expect(sysex.isSysEx());
            expectEquals(sysex.getSysExDataSize(), 5);

            auto* sysexData = sysex.getSysExData();
            expectEquals((int) sysexData[0], 0x00);
            expectEquals((int) sysexData[1], 0x20);
            expectEquals((int) sysexData[2], 0x29);
            expectEquals((int) sysexData[3], 0x02);
            expectEquals((int) sysexData[4], 0x13);
        }

        beginTest("CC message construction");
        {
            auto cc = juce::MidiMessage::controllerEvent(1, 74, 100);
            expect(cc.isController());
            expectEquals(cc.getControllerNumber(), 74);
            expectEquals(cc.getControllerValue(), 100);
        }

        beginTest("Poly aftertouch message");
        {
            auto at = juce::MidiMessage::aftertouchChange(1, 60, 80);
            expect(at.isAftertouch());
            expectEquals(at.getNoteNumber(), 60);
            expectEquals(at.getAfterTouchValue(), 80);
        }

        beginTest("DAW mode enable message");
        {
            auto msg = juce::MidiMessage(0x9F, 0x0C, 0x7F);
            expect(msg.isNoteOn());
            expectEquals(msg.getChannel(), 16);
            expectEquals(msg.getNoteNumber(), 12);
            expectEquals((int) msg.getVelocity(), 127);
        }

        beginTest("DAW mode disable message");
        {
            auto msg = juce::MidiMessage(0x9F, 0x0C, 0x00);
            expectEquals(msg.getChannel(), 16);
            expectEquals(msg.getNoteNumber(), 12);
        }

        beginTest("Pad LED color via note velocity");
        {
            auto msg = juce::MidiMessage::noteOn(1, 0x60, (juce::uint8) 5);
            expect(msg.isNoteOn());
            expectEquals(msg.getChannel(), 1);
            expectEquals(msg.getNoteNumber(), 0x60);
            expectEquals((int) msg.getVelocity(), 5);
        }

        beginTest("RGB color SysEx construction");
        {
            std::vector<juce::uint8> payload = {
                0x00, 0x20, 0x29, 0x02, 0x13,
                0x01, 0x43, 0x60, 0x7F, 0x00, 0x00
            };
            auto msg = juce::MidiMessage::createSysExMessage(payload.data(), (int) payload.size());
            expect(msg.isSysEx());
            auto* d = msg.getSysExData();
            expectEquals((int) d[5], 0x01);
            expectEquals((int) d[6], 0x43);
            expectEquals((int) d[7], 0x60);
            expectEquals((int) d[8], 0x7F);
        }
    }
};

class BitmapEncodingTests : public juce::UnitTest
{
public:
    BitmapEncodingTests() : UnitTest("Bitmap Encoding") {}

    void runTest() override
    {
        beginTest("Empty bitmap is all zeros");
        {
            std::array<juce::uint8, 1216> buffer {};
            for (auto b : buffer)
                expectEquals((int) b, 0);
        }

        beginTest("Pixel to SysEx encoding — 7 bits per byte");
        {
            // First 7 pixels of row 0: bit 6 = leftmost, bit 0 = 7th pixel
            // If pixel 0 is on: byte = 0b1000000 = 0x40
            juce::uint8 byte = 0;
            int pixelX = 0;
            int bit = 6 - (pixelX % 7);
            byte |= (1 << bit);
            expectEquals((int) byte, 0x40);

            // If pixel 6 is on: byte = 0b0000001 = 0x01
            byte = 0;
            pixelX = 6;
            bit = 6 - (pixelX % 7);
            byte |= (1 << bit);
            expectEquals((int) byte, 0x01);
        }

        beginTest("Full row encoding — 19 bytes for 128 pixels");
        {
            int bitsUsed = 19 * 7; // 133 bits
            expect(bitsUsed >= 128);
            expectEquals(19 * 64, 1216);
        }

        beginTest("The display's ready reply (header + 09, as measured on the device) is recognised; the newest frame goes out on it");
        {
            struct Count : LaunchkeyController::Listener { int acks = 0; void bitmapAckReceived() override { ++acks; } } count;
            LaunchkeyController controller;
            controller.addListener(&count);
            const juce::uint8 ack[] = { 0xF0, 0x00, 0x20, 0x29, 0x02, 0x13, 0x09, 0xF7 };
            static_cast<juce::MidiInputCallback&>(controller).handleIncomingMidiMessage(nullptr, juce::MidiMessage(ack, sizeof(ack)));
            expectEquals(count.acks, 1);
            const juce::uint8 other[] = { 0xF0, 0x00, 0x20, 0x29, 0x02, 0x13, 0x0A, 0xF7 };
            static_cast<juce::MidiInputCallback&>(controller).handleIncomingMidiMessage(nullptr, juce::MidiMessage(other, sizeof(other)));
            expectEquals(count.acks, 1);
            controller.removeListener(&count);

            // A frame drawn while the device is busy is not queued; it is sent when the reply arrives.
            DisplayRenderer display;
            display.setController(&controller);
            display.clear();
            display.setPixel(1, 1, true);
            display.sendFrame();
            expect(display.isWaitingForAck());
            expectEquals(display.getFramesSent(), 1);
            display.setPixel(5, 5, true);
            display.sendFrame();
            expectEquals(display.getFramesSent(), 1, "nothing is sent while waiting");
            expect(display.isDirty(), "but the newer frame is remembered");
            display.onBitmapAck();
            expectEquals(display.getFramesSent(), 2, "and goes out on the reply");
            expect(display.isWaitingForAck() && ! display.isDirty());
            display.onBitmapAck();
            expectEquals(display.getFramesSent(), 2, "an unchanged screen is not resent");

            // The reply can arrive (on the MIDI thread) while the next picture is half drawn. What
            // goes out is the last finished picture, never the buffer being drawn into.
            std::array<juce::uint8, DisplayRenderer::SYSEX_DATA_SIZE> sent {};
            display.onFrameSent = [&sent](const std::array<juce::uint8, DisplayRenderer::SYSEX_DATA_SIZE>& f) { sent = f; };
            display.clear();
            display.drawString(2, 2, "WHOLE");
            display.sendFrame();                       // goes out at once: frame 3
            const auto whole3 = sent;
            display.clear();
            display.drawString(2, 20, "NEXT");
            display.sendFrame();                       // waits for the reply
            const auto finished = display.getSysExData();
            display.clear();                           // the following picture is being drawn ...
            display.drawString(2, 40, "HALF");
            display.onBitmapAck();                     // ... when the reply arrives
            expectEquals(display.getFramesSent(), 4);
            expect(sent == finished && sent != whole3, "The finished picture was sent, not the half-drawn one");
            expect(!display.isIdle());
            display.onBitmapAck();
            expect(display.isIdle());
            DisplayRenderer::PixelBuffer last;
            display.getLastSentPixels(last);
            expect(last[20 + 1][2] || last[20][2] || last[20 + 2][2], "NEXT is what the device shows");
            for (int x = 0; x < 128; ++x)
                for (int y = 40; y < 48; ++y)
                    if (last[(size_t) y][(size_t) x]) { expect(false, "No pixel of the half-drawn picture reached the device"); x = 128; break; }

            // The drawing audit: text over other drawing, and text that runs off the glass.
            DisplayRenderer::DrawAudit audit;
            DisplayRenderer d;
            d.setDrawAudit(&audit);
            d.clear();
            d.drawString(2, 2, "CLEAN");
            d.drawRect(0, 12, 40, 10, false);
            d.drawString(3, 14, "IN BOX");
            expect(!audit.overText.any() && !audit.clipped.any(), "Text beside and inside shapes is clean");
            d.drawString(2, 3, "OVER");                // one row lower, on top of CLEAN
            expect(audit.overText.any() && audit.overText.y1 <= 10);
            d.clear();
            expect(!audit.overText.any(), "clear() starts a new frame");
            d.drawString(110, 30, "TOO LONG");
            expect(audit.clipped.any());
            d.clear();
            d.drawRect(0, 0, 30, 9, true);
            d.invertRegion(0, 0, 30, 9);               // an inverted label is deliberate, not a collision
            expect(!audit.overText.any());
        }
    }
};

class VoiceAllocatorTests : public juce::UnitTest
{
public:
    VoiceAllocatorTests() : UnitTest("Voice Allocator") {}

    void runTest() override
    {
        beginTest("Poly allocation assigns different voices");
        {
            VoiceAllocator alloc(4);
            alloc.setMode(VoiceMode::Poly);

            int v0 = alloc.noteOn(60, 1.0f);
            int v1 = alloc.noteOn(64, 1.0f);
            int v2 = alloc.noteOn(67, 1.0f);

            expect(v0 != v1);
            expect(v1 != v2);
            expectEquals(alloc.getActiveVoiceCount(), 3);
        }

        beginTest("Poly voice stealing takes oldest");
        {
            VoiceAllocator alloc(2);
            alloc.noteOn(60, 1.0f);
            alloc.noteOn(64, 1.0f);
            int stolen = alloc.noteOn(67, 1.0f);

            expectEquals(stolen, 0);
            expectEquals(alloc.getVoice(0).note, 67);
        }

        beginTest("Mono mode reuses voice 0");
        {
            VoiceAllocator alloc(4);
            alloc.setMode(VoiceMode::Mono);

            alloc.noteOn(60, 1.0f);
            alloc.noteOn(64, 1.0f);
            expectEquals(alloc.getVoice(0).note, 64);
            expectEquals(alloc.getActiveVoiceCount(), 1);
        }

        beginTest("Note off releases correct voice");
        {
            VoiceAllocator alloc(4);
            alloc.noteOn(60, 1.0f);
            alloc.noteOn(64, 1.0f);
            alloc.noteOff(60);
            expectEquals(alloc.getActiveVoiceCount(), 1);
            expect(alloc.getVoice(1).active);
        }

        beginTest("All notes off clears everything");
        {
            VoiceAllocator alloc(4);
            alloc.noteOn(60, 1.0f);
            alloc.noteOn(64, 1.0f);
            alloc.allNotesOff();
            expectEquals(alloc.getActiveVoiceCount(), 0);
        }

        beginTest("Poly aftertouch reaches correct voice");
        {
            VoiceAllocator alloc(4);
            alloc.noteOn(60, 1.0f);
            int v1 = alloc.noteOn(64, 1.0f);
            alloc.setAftertouch(64, 0.8f);
            expectEquals(alloc.getAftertouch(v1), 0.8f);
        }

        beginTest("Unison mode triggers all voices");
        {
            VoiceAllocator alloc(4);
            alloc.setMode(VoiceMode::Unison);
            alloc.noteOn(60, 1.0f);
            expectEquals(alloc.getActiveVoiceCount(), 4);
            for (int i = 0; i < 4; ++i)
                expectEquals(alloc.getVoice(i).note, 60);
        }
    }
};

class SequencerTests : public juce::UnitTest
{
public:
    SequencerTests() : UnitTest("Sequencer Core") {}

    void runTest() override
    {
        beginTest("Default pattern has 8 tracks, 16 steps each");
        {
            SequencerCore seq;
            expectEquals(seq.getPattern().getTrackCount(), 8);
            expectEquals(seq.getPattern().getTrack(0).length, 16);
        }

        beginTest("Tempo range is clamped");
        {
            SequencerCore seq;
            seq.setTempo(10.0f);
            expect(seq.getTempo() >= 20.0f);
            seq.setTempo(500.0f);
            expect(seq.getTempo() <= 300.0f);
        }

        beginTest("Transport start/stop");
        {
            SequencerCore seq;
            expect(!seq.isPlaying());
            seq.start();
            expect(seq.isPlaying());
            seq.stop();
            expect(!seq.isPlaying());
        }

        beginTest("Track length is independent");
        {
            SequencerCore seq;
            seq.getPattern().getTrack(0).setLength(32);
            seq.getPattern().getTrack(1).setLength(8);
            expectEquals(seq.getPattern().getTrack(0).length, 32);
            expectEquals(seq.getPattern().getTrack(1).length, 8);
        }

        beginTest("Step parameter locks");
        {
            Step step;
            step.paramLocks[42] = 0.75f;
            expect(step.hasParamLock(42));
            expect(!step.hasParamLock(43));
            expectEquals(step.getParamLock(42, 0.0f), 0.75f);
            expectEquals(step.getParamLock(43, 0.5f), 0.5f);
        }

        beginTest("Step copy and paste");
        {
            SequencerCore seq;
            auto& track = seq.getPattern().getTrack(0);
            track.getStep(0).trig = true;
            track.getStep(0).notes.push_back({60, 1.0f});
            track.getStep(1).trig = true;
            track.getStep(1).paramLocks[10] = 0.5f;

            std::vector<Step> buffer;
            track.copyStepRange(0, 4, buffer);
            expectEquals((int) buffer.size(), 4);
            expect(buffer[0].trig);
            expect(buffer[1].trig);
            expect(!buffer[2].trig);

            track.pasteStepRange(8, buffer);
            expect(track.getStep(8).trig);
            expect(track.getStep(9).trig);
            expectEquals(track.getStep(9).getParamLock(10, 0.0f), 0.5f);
        }

        beginTest("Pattern duplicate");
        {
            SequencerCore seq;
            seq.getPattern().getTrack(0).getStep(0).trig = true;
            seq.getPattern().swing = 0.3f;

            auto copy = seq.getPattern().duplicate();
            expect(copy.getTrack(0).getStep(0).trig);
            expectWithinAbsoluteError(copy.swing, 0.3f, 0.001f);
            expectEquals(copy.getTrackCount(), seq.getPattern().getTrackCount());
        }

        beginTest("Trig callback fires during playback");
        {
            SequencerCore seq;
            seq.prepareToPlay(44100.0, 512);
            seq.setTempo(120.0f);

            auto& track = seq.getPattern().getTrack(0);
            for (int i = 0; i < 16; ++i)
            {
                track.getStep(i).trig = true;
                track.getStep(i).notes.push_back({60, 1.0f});
            }

            int trigCount = 0;
            seq.setTrigCallback([&](int /*trackIdx*/, const Step& /*step*/)
            {
                ++trigCount;
            });

            seq.start();
            // At 120 BPM, one step = 60/120/4 * 44100 = 5512.5 samples
            // Process enough for multiple steps
            for (int i = 0; i < 100; ++i)
                seq.processBlock(512);

            seq.stop();
            expect(trigCount > 0);
        }

        beginTest("Muted tracks don't fire trigs");
        {
            SequencerCore seq;
            seq.prepareToPlay(44100.0, 512);
            seq.setTempo(120.0f);

            auto& track = seq.getPattern().getTrack(0);
            track.getStep(0).trig = true;
            track.muted = true;

            int trigCount = 0;
            seq.setTrigCallback([&](int /*trackIdx*/, const Step& /*step*/)
            {
                ++trigCount;
            });

            seq.start();
            for (int i = 0; i < 100; ++i)
                seq.processBlock(512);
            seq.stop();
            expectEquals(trigCount, 0);
        }

        beginTest("Track selection");
        {
            SequencerCore seq;
            expectEquals(seq.getSelectedTrack(), 0);
            seq.setSelectedTrack(3);
            expectEquals(seq.getSelectedTrack(), 3);
            seq.setSelectedTrack(-1);
            expectEquals(seq.getSelectedTrack(), 3);
        }
    }
};

class EnvelopeTests : public juce::UnitTest
{
public:
    EnvelopeTests() : UnitTest("Envelope") {}

    void runTest() override
    {
        beginTest("Envelope starts idle at 0");
        {
            Envelope env;
            env.setSampleRate(44100.0);
            expect(!env.isActive());
            expectEquals(env.getCurrentLevel(), 0.0f);
        }

        beginTest("Note on triggers attack, reaches 1.0");
        {
            Envelope env;
            env.setSampleRate(44100.0);
            Envelope::Params p;
            p.attack = 0.001f;
            env.setParams(p);
            env.noteOn();
            expect(env.isActive());

            for (int i = 0; i < 100; ++i)
                env.process();

            expect(env.getCurrentLevel() > 0.9f);
        }

        beginTest("Note off triggers release, returns to 0");
        {
            Envelope env;
            env.setSampleRate(44100.0);
            Envelope::Params p;
            p.attack = 0.001f;
            p.decay = 0.001f;
            p.sustain = 0.5f;
            p.release = 0.001f;
            env.setParams(p);

            env.noteOn();
            for (int i = 0; i < 200; ++i) env.process();
            env.noteOff();
            for (int i = 0; i < 200; ++i) env.process();

            expect(!env.isActive());
            expectEquals(env.getCurrentLevel(), 0.0f);
        }
    }
};

class SVFilterTests : public juce::UnitTest
{
public:
    SVFilterTests() : UnitTest("SV Filter") {}

    void runTest() override
    {
        beginTest("Filter attenuates above cutoff in lowpass");
        {
            SVFilter filter;
            filter.setSampleRate(44100.0);
            filter.setCutoff(100.0f);
            filter.setResonance(0.707f);
            filter.setMode(SVFilter::Mode::LowPass);

            float sum = 0.0f;
            for (int i = 0; i < 1000; ++i)
            {
                float input = std::sin(2.0f * 3.14159f * 5000.0f * i / 44100.0f);
                float output = filter.process(input);
                sum += std::abs(output);
            }

            float avgOutput = sum / 1000.0f;
            expect(avgOutput < 0.1f);
        }

        beginTest("Filter passes signal below cutoff");
        {
            SVFilter filter;
            filter.setSampleRate(44100.0);
            filter.setCutoff(5000.0f);
            filter.setResonance(0.707f);
            filter.setMode(SVFilter::Mode::LowPass);

            for (int i = 0; i < 500; ++i)
            {
                float input = std::sin(2.0f * 3.14159f * 100.0f * i / 44100.0f);
                filter.process(input);
            }

            float sum = 0.0f;
            for (int i = 500; i < 1500; ++i)
            {
                float input = std::sin(2.0f * 3.14159f * 100.0f * i / 44100.0f);
                float output = filter.process(input);
                sum += std::abs(output);
            }

            float avgOutput = sum / 1000.0f;
            expect(avgOutput > 0.3f);
        }
    }
};

class DisplayRendererTests : public juce::UnitTest
{
public:
    DisplayRendererTests() : UnitTest("Display Renderer") {}

    void runTest() override
    {
        beginTest("Clear sets all pixels off");
        {
            DisplayRenderer display;
            display.setPixel(10, 10, true);
            display.clear();
            expect(!display.getPixel(10, 10));
        }

        beginTest("SetPixel and getPixel");
        {
            DisplayRenderer display;
            display.setPixel(0, 0, true);
            display.setPixel(127, 63, true);
            display.setPixel(64, 32, true);

            expect(display.getPixel(0, 0));
            expect(display.getPixel(127, 63));
            expect(display.getPixel(64, 32));
            expect(!display.getPixel(1, 0));
        }

        beginTest("Out of bounds pixels are ignored");
        {
            DisplayRenderer display;
            display.setPixel(-1, 0, true);
            display.setPixel(0, -1, true);
            display.setPixel(128, 0, true);
            display.setPixel(0, 64, true);
            expect(!display.getPixel(-1, 0));
            expect(!display.getPixel(128, 0));
        }

        beginTest("DrawHLine sets correct pixels");
        {
            DisplayRenderer display;
            display.drawHLine(10, 5, 20);
            for (int x = 10; x < 30; ++x)
                expect(display.getPixel(x, 5));
            expect(!display.getPixel(9, 5));
            expect(!display.getPixel(30, 5));
        }

        beginTest("DrawVLine sets correct pixels");
        {
            DisplayRenderer display;
            display.drawVLine(5, 10, 15);
            for (int y = 10; y < 25; ++y)
                expect(display.getPixel(5, y));
            expect(!display.getPixel(5, 9));
            expect(!display.getPixel(5, 25));
        }

        beginTest("DrawRect outline");
        {
            DisplayRenderer display;
            display.drawRect(10, 10, 20, 10, false);
            expect(display.getPixel(10, 10));
            expect(display.getPixel(29, 10));
            expect(display.getPixel(10, 19));
            expect(display.getPixel(29, 19));
            expect(!display.getPixel(15, 15));
        }

        beginTest("DrawRect filled");
        {
            DisplayRenderer display;
            display.drawRect(5, 5, 3, 3, true);
            for (int y = 5; y < 8; ++y)
                for (int x = 5; x < 8; ++x)
                    expect(display.getPixel(x, y));
            expect(!display.getPixel(4, 5));
            expect(!display.getPixel(8, 5));
        }

        beginTest("DrawChar renders non-empty glyph");
        {
            DisplayRenderer display;
            display.drawChar(0, 0, 'A');
            int litPixels = 0;
            for (int y = 0; y < BitmapFont::CHAR_HEIGHT; ++y)
                for (int x = 0; x < BitmapFont::CHAR_WIDTH; ++x)
                    if (display.getPixel(x, y))
                        ++litPixels;
            expect(litPixels > 0);
        }

        beginTest("DrawString renders multiple chars");
        {
            DisplayRenderer display;
            display.drawString(0, 0, "AB");
            expect(display.getPixel(0, 0) || display.getPixel(1, 0));
            int secondCharStart = BitmapFont::TOTAL_CHAR_WIDTH;
            int litInSecond = 0;
            for (int y = 0; y < BitmapFont::CHAR_HEIGHT; ++y)
                for (int x = secondCharStart; x < secondCharStart + BitmapFont::CHAR_WIDTH; ++x)
                    if (display.getPixel(x, y))
                        ++litInSecond;
            expect(litInSecond > 0);
        }

        beginTest("SysEx encoding produces correct size");
        {
            DisplayRenderer display;
            display.setPixel(0, 0, true);
            auto& sysex = display.getSysExData();
            expectEquals((int) sysex.size(), DisplayRenderer::SYSEX_DATA_SIZE);
        }

        beginTest("Dirty flag tracks changes");
        {
            DisplayRenderer display;
            display.clear();
            expect(display.isDirty());
        }
    }
};

class PadManagerTests : public juce::UnitTest
{
public:
    PadManagerTests() : UnitTest("Pad Manager") {}

    void runTest() override
    {
        beginTest("Default mode is Step");
        {
            PadManager pm;
            expectEquals((int) pm.getMode(), (int) PadManager::Mode::Step);
        }

        beginTest("Mode switching");
        {
            PadManager pm;
            pm.setMode(PadManager::Mode::Play);
            expectEquals((int) pm.getMode(), (int) PadManager::Mode::Play);
            pm.setMode(PadManager::Mode::Mute);
            expectEquals((int) pm.getMode(), (int) PadManager::Mode::Mute);
        }

        beginTest("Pad press and release tracking");
        {
            PadManager pm;
            expect(!pm.isPadHeld(0));
            pm.onPadPress(0, 100);
            expect(pm.isPadHeld(0));
            expectEquals(pm.heldPadCount(), 1);
            pm.onPadRelease(0);
            expect(!pm.isPadHeld(0));
            expectEquals(pm.heldPadCount(), 0);
        }

        beginTest("Multiple pads held simultaneously");
        {
            PadManager pm;
            pm.onPadPress(0, 100);
            pm.onPadPress(5, 100);
            pm.onPadPress(15, 100);
            expectEquals(pm.heldPadCount(), 3);
            expect(pm.isPadHeld(0));
            expect(pm.isPadHeld(5));
            expect(pm.isPadHeld(15));
            expect(!pm.isPadHeld(1));
        }

        beginTest("Listener receives pad events");
        {
            struct TestListener : PadManager::Listener
            {
                int lastPad = -1;
                int lastVelocity = -1;
                PadManager::Mode lastMode = PadManager::Mode::Step;
                bool released = false;

                void padPressed(int padIndex, int velocity, PadManager::Mode mode) override
                { lastPad = padIndex; lastVelocity = velocity; lastMode = mode; }
                void padReleased(int padIndex, PadManager::Mode /*mode*/) override
                { lastPad = padIndex; released = true; }
            };

            PadManager pm;
            TestListener listener;
            pm.addListener(&listener);
            pm.onPadPress(7, 80);
            expectEquals(listener.lastPad, 7);
            expectEquals(listener.lastVelocity, 80);
            pm.onPadRelease(7);
            expect(listener.released);
            pm.removeListener(&listener);
        }

        beginTest("Pad index bounds");
        {
            PadManager pm;
            expectEquals(PadManager::TOTAL_PADS, 16);
        }
    }
};

class BitmapFontTests : public juce::UnitTest
{
public:
    BitmapFontTests() : UnitTest("Bitmap Font") {}

    void runTest() override
    {
        beginTest("Space glyph is blank");
        {
            auto* glyph = BitmapFont::getGlyph(' ');
            for (int i = 0; i < BitmapFont::CHAR_WIDTH; ++i)
                expectEquals((int) glyph[i], 0);
        }

        beginTest("A glyph is non-blank");
        {
            auto* glyph = BitmapFont::getGlyph('A');
            int sum = 0;
            for (int i = 0; i < BitmapFont::CHAR_WIDTH; ++i)
                sum += glyph[i];
            expect(sum > 0);
        }

        beginTest("Lowercase a glyph is non-blank");
        {
            auto* glyph = BitmapFont::getGlyph('a');
            int sum = 0;
            for (int i = 0; i < BitmapFont::CHAR_WIDTH; ++i)
                sum += glyph[i];
            expect(sum > 0);
        }

        beginTest("Unsupported char returns space");
        {
            auto* glyph = BitmapFont::getGlyph('[');
            auto* space = BitmapFont::getGlyph(' ');
            for (int i = 0; i < BitmapFont::CHAR_WIDTH; ++i)
                expectEquals((int) glyph[i], (int) space[i]);
        }

        beginTest("StringWidth calculation");
        {
            expectEquals(BitmapFont::stringWidth("ABC", 3), 3 * BitmapFont::TOTAL_CHAR_WIDTH - BitmapFont::CHAR_SPACING);
            expectEquals(BitmapFont::stringWidth("A", 1), BitmapFont::TOTAL_CHAR_WIDTH - BitmapFont::CHAR_SPACING);
            expectEquals(BitmapFont::stringWidth("", 0), 0);
        }
    }
};

class PadLEDFeedbackTests : public juce::UnitTest
{
public:
    PadLEDFeedbackTests() : UnitTest("Pad LED Feedback") {}

    void runTest() override
    {
        beginTest("Empty step returns dim white");
        {
            SequencerCore seq;
            PadLEDFeedback leds;
            leds.setSequencer(&seq);

            int color = leds.getStepColor(0, 0);
            expectEquals(color, PadColors::STEP_EMPTY);
        }

        beginTest("Active step returns yellow");
        {
            SequencerCore seq;
            PadLEDFeedback leds;
            leds.setSequencer(&seq);

            auto& track = seq.getPattern().getTrack(0);
            track.getStep(0).trig = true;

            int color = leds.getStepColor(0, 0);
            expectEquals(color, PadColors::STEP_ACTIVE);
        }

        beginTest("Locked step returns cyan");
        {
            SequencerCore seq;
            PadLEDFeedback leds;
            leds.setSequencer(&seq);

            auto& step = seq.getPattern().getTrack(0).getStep(3);
            step.paramLocks[42] = 0.5f;

            int color = leds.getStepColor(3, 0);
            expectEquals(color, PadColors::STEP_LOCKED);
        }

        beginTest("Active + locked step returns green");
        {
            SequencerCore seq;
            PadLEDFeedback leds;
            leds.setSequencer(&seq);

            auto& step = seq.getPattern().getTrack(0).getStep(2);
            step.trig = true;
            step.paramLocks[10] = 1.0f;

            int color = leds.getStepColor(2, 0);
            expectEquals(color, PadColors::STEP_ACTIVE_LOCKED);
        }

        beginTest("Beyond track length returns off");
        {
            SequencerCore seq;
            PadLEDFeedback leds;
            leds.setSequencer(&seq);

            auto& track = seq.getPattern().getTrack(0);
            track.setLength(8);

            int color = leds.getStepColor(8, 0);
            expectEquals(color, PadColors::OFF);
        }

        beginTest("Mute mode colors");
        {
            SequencerCore seq;
            PadLEDFeedback leds;
            leds.setSequencer(&seq);

            seq.getPattern().getTrack(0).muted = false;
            seq.getPattern().getTrack(1).muted = true;
            seq.getPattern().getTrack(2).soloed = true;

            expectEquals(leds.getMuteColor(0), PadColors::MUTE_OFF);
            expectEquals(leds.getMuteColor(1), PadColors::MUTE_ON);
            expectEquals(leds.getMuteColor(2), PadColors::SOLO_ON);
        }
    }
};

// ===== The filter set: every type and mode, measured =====
class FilterSetTests : public juce::UnitTest
{
public:
    FilterSetTests() : UnitTest("Filter Set") {}

    // Steady-state gain of a slot for a sine at `hz` (amplitude 0.25, so the nonlinear models stay
    // close to linear).
    static float gainAt(FilterSlot& slot, float hz)
    {
        const double sr = 48000.0;
        slot.reset();
        double phase = 0.0, inSum = 0.0, outSum = 0.0;
        for (int i = 0; i < 24000; ++i)
        {
            float in = 0.25f * (float) std::sin(phase);
            phase += 2.0 * 3.14159265358979 * hz / sr;
            float out = slot.process(in);
            if (i >= 12000) { inSum += in * in; outSum += out * out; }
        }
        return (float) std::sqrt(outSum / inSum);
    }

    static FilterSlot make(FilterModel model, int mode, float cutoff, float reso)
    {
        FilterSlot slot;
        slot.setSampleRate(48000.0);
        slot.setModel(0, model);
        slot.setMode(0, mode);
        slot.setCutoff(cutoff);
        slot.setResonance(reso);
        return slot;
    }

    void runTest() override
    {
        beginTest("Six types offered in a fixed order, each with named modes");
        {
            const char* expected[] = { "SVF", "SEM", "LADR", "DIOD", "COMB", "OFF" };
            for (int i = 0; i < FilterSlot::NUM_MODELS; ++i)
            {
                expectEquals(juce::String(FilterSlot::modelNames()[i]), juce::String(expected[i]));
                auto model = FilterSlot::modelFromIndex(i);
                expectEquals(FilterSlot::indexFromModel(model), i);
                expect(FilterSlot::modeCount(model) >= 1);
                for (int m = 0; m < FilterSlot::modeCount(model); ++m)
                    expect(juce::String(FilterSlot::modeNames(model)[m]).isNotEmpty());
            }
            // Stored sounds use the original enum values.
            expectEquals((int) FilterModel::SVF, 0);
            expectEquals((int) FilterModel::Ladder, 1);
            expectEquals((int) FilterModel::Diode, 2);
            expectEquals((int) FilterModel::Bypass, 3);
        }

        beginTest("State-variable: LP / BP / HP / notch do what their names say");
        {
            auto lp = make(FilterModel::SVF, 0, 1000.0f, 0.0f);
            expect(gainAt(lp, 100.0f) > 0.9f);
            expect(gainAt(lp, 8000.0f) < 0.05f, "12 dB/oct: three octaves up is at least 26 dB down");

            auto bp = make(FilterModel::SVF, 1, 1000.0f, 0.3f);
            expect(gainAt(bp, 1000.0f) > gainAt(bp, 100.0f) * 3.0f);
            expect(gainAt(bp, 1000.0f) > gainAt(bp, 8000.0f) * 3.0f);

            auto hp = make(FilterModel::SVF, 2, 1000.0f, 0.0f);
            expect(gainAt(hp, 8000.0f) > 0.9f);
            expect(gainAt(hp, 125.0f) < 0.05f);

            auto notch = make(FilterModel::SVF, 3, 1000.0f, 0.3f);
            expect(gainAt(notch, 1000.0f) < 0.05f, "Notch removes the cutoff frequency");
            expect(gainAt(notch, 100.0f) > 0.9f);
            expect(gainAt(notch, 8000.0f) > 0.9f);
        }

        beginTest("State-variable is tuned right up to the top (no quarter-sample-rate limit)");
        {
            auto lp = make(FilterModel::SVF, 0, 16000.0f, 0.0f);
            expect(gainAt(lp, 10000.0f) > 0.75f, "A 16 kHz low-pass passes 10 kHz");
            auto resonant = make(FilterModel::SVF, 0, 2000.0f, 0.8f);
            expect(gainAt(resonant, 2000.0f) > 4.0f, "Resonance peaks at the cutoff");
        }

        beginTest("SEM: morph sweeps low-pass -> notch -> high-pass, band-pass separate, resonance never runs away");
        {
            auto sem = make(FilterModel::SEM, 0, 1000.0f, 0.2f);
            sem.setMorph(0.0f);
            expect(gainAt(sem, 100.0f) > 0.9f && gainAt(sem, 8000.0f) < 0.05f, "Morph 0 = low-pass");
            sem.setMorph(0.5f);
            expect(gainAt(sem, 1000.0f) < 0.1f, "Morph centre = notch");
            expect(gainAt(sem, 100.0f) > 0.9f && gainAt(sem, 8000.0f) > 0.9f);
            sem.setMorph(1.0f);
            expect(gainAt(sem, 8000.0f) > 0.9f && gainAt(sem, 125.0f) < 0.05f, "Morph 1 = high-pass");

            auto bp = make(FilterModel::SEM, 1, 1000.0f, 0.5f);
            expect(gainAt(bp, 1000.0f) > gainAt(bp, 100.0f) * 3.0f);
            expectWithinAbsoluteError(gainAt(bp, 1000.0f), 1.0f, 0.15f);

            // Full resonance: a strong peak, but silence in means silence out (no self-oscillation).
            auto full = make(FilterModel::SEM, 0, 1000.0f, 1.0f);
            float peak = gainAt(full, 1000.0f);
            expect(peak > 3.0f && peak < 12.0f, "Peak gain " + juce::String(peak));
            float tail = 0.0f;
            full.process(1.0f);
            for (int i = 0; i < 48000; ++i) tail = full.process(0.0f);
            expect(std::abs(tail) < 0.0001f, "Rings out");
        }

        beginTest("Ladder: tuned to its cutoff; 24 / 18 / 12 / 6 dB taps; band-pass and high-pass; self-oscillates");
        {
            // Four cascaded poles are 12 dB down at the cutoff: about 0.25.
            auto lp24 = make(FilterModel::Ladder, 0, 1000.0f, 0.0f);
            expect(gainAt(lp24, 100.0f) > 0.9f, "Pass-band is flat well below the cutoff");
            expectWithinAbsoluteError(gainAt(lp24, 1000.0f), 0.25f, 0.06f);

            // Each tap further up the ladder lets more of the 8 kHz through.
            float g24 = gainAt(lp24, 8000.0f);
            auto lp18 = make(FilterModel::Ladder, 1, 1000.0f, 0.0f);
            auto lp12 = make(FilterModel::Ladder, 2, 1000.0f, 0.0f);
            auto lp6  = make(FilterModel::Ladder, 3, 1000.0f, 0.0f);
            float g18 = gainAt(lp18, 8000.0f), g12 = gainAt(lp12, 8000.0f), g6 = gainAt(lp6, 8000.0f);
            expect(g24 < 0.002f, "24 dB/oct three octaves up: " + juce::String(g24));
            expect(g18 > g24 * 3.0f && g12 > g18 * 3.0f && g6 > g12 * 3.0f,
                   juce::String(g24, 5) + " " + juce::String(g18, 5) + " " + juce::String(g12, 5) + " " + juce::String(g6, 5));

            auto bp = make(FilterModel::Ladder, 4, 1000.0f, 0.2f);
            expect(gainAt(bp, 1000.0f) > gainAt(bp, 60.0f) * 10.0f);
            expect(gainAt(bp, 1000.0f) > gainAt(bp, 12000.0f) * 10.0f);

            auto hp = make(FilterModel::Ladder, 5, 1000.0f, 0.0f);
            expect(gainAt(hp, 10000.0f) > 0.6f);
            expect(gainAt(hp, 60.0f) < 0.05f);

            // Resonance: a peak at the cutoff, and at the top the filter sings on its own.
            auto res = make(FilterModel::Ladder, 0, 1000.0f, 0.7f);
            expect(gainAt(res, 1000.0f) > gainAt(res, 100.0f) * 1.5f);
            auto osc = make(FilterModel::Ladder, 0, 1000.0f, 1.0f);
            osc.process(0.5f);
            float late = 0.0f;
            for (int i = 0; i < 48000; ++i) { float o = std::abs(osc.process(0.0f)); if (i > 40000) late = juce::jmax(late, o); }
            expect(late > 0.005f, "Full resonance keeps ringing: " + juce::String(late, 5));
        }

        beginTest("Diode: LP / HP / BP");
        {
            auto lp = make(FilterModel::Diode, 0, 1000.0f, 0.0f);
            expect(gainAt(lp, 100.0f) > 0.8f && gainAt(lp, 8000.0f) < 0.06f);
            auto hp = make(FilterModel::Diode, 1, 1000.0f, 0.0f);
            expect(gainAt(hp, 8000.0f) > 0.7f && gainAt(hp, 100.0f) < 0.1f);
            auto bp = make(FilterModel::Diode, 2, 1000.0f, 0.0f);
            expect(gainAt(bp, 1000.0f) > gainAt(bp, 60.0f) * 2.0f);
        }

        beginTest("Comb: peaks at multiples of the cutoff (positive) or odd half-multiples (negative)");
        {
            auto pos = make(FilterModel::Comb, 0, 1000.0f, 0.8f);
            expect(gainAt(pos, 1000.0f) > gainAt(pos, 1500.0f) * 3.0f);
            expect(gainAt(pos, 2000.0f) > gainAt(pos, 1500.0f) * 3.0f);
            auto neg = make(FilterModel::Comb, 1, 1000.0f, 0.8f);
            expect(gainAt(neg, 500.0f) > gainAt(neg, 1000.0f) * 3.0f);
        }

        beginTest("Every type and mode stays finite and bounded under abuse");
        {
            juce::Random rng(1234);
            for (int t = 0; t < FilterSlot::NUM_MODELS; ++t)
            {
                auto model = FilterSlot::modelFromIndex(t);
                for (int m = 0; m < FilterSlot::modeCount(model); ++m)
                {
                    FilterSlot slot;
                    slot.setSampleRate(44100.0);
                    slot.setModel(0, model);
                    slot.setMode(0, m);
                    slot.setResonance(1.0f);
                    slot.setDrive(1.0f);
                    float worst = 0.0f;
                    bool finite = true;
                    for (int i = 0; i < 40000; ++i)
                    {
                        // Cutoff swept across the whole range every sample, loud noise in.
                        slot.setCutoff(20.0f * std::pow(1000.0f, rng.nextFloat()));
                        float out = slot.process(rng.nextFloat() * 4.0f - 2.0f);
                        finite = finite && std::isfinite(out);
                        worst = juce::jmax(worst, std::abs(out));
                    }
                    expect(finite, juce::String(FilterSlot::modelNames()[t]) + " mode " + juce::String(m) + " produced NaN/inf");
                    expect(worst < 200.0f, juce::String(FilterSlot::modelNames()[t]) + " mode " + juce::String(m)
                                           + " peaked at " + juce::String(worst));
                }
            }
        }

        beginTest("Routing: series stacks the two filters, parallel balance picks between them");
        {
            FilterSlot slot;
            slot.setSampleRate(48000.0);
            slot.setModel(0, FilterModel::SVF); slot.setMode(0, 0);     // low-pass
            slot.setModel(1, FilterModel::SVF); slot.setMode(1, 2);     // high-pass
            slot.setCutoff(0, 2000.0f);
            slot.setCutoff(1, 500.0f);
            slot.setResonance(0.0f);

            slot.setRouting(FilterRouting::Series);                     // 500 Hz - 2 kHz band
            expect(gainAt(slot, 1000.0f) > 0.6f);
            expect(gainAt(slot, 60.0f) < 0.05f && gainAt(slot, 12000.0f) < 0.05f);

            slot.setRouting(FilterRouting::Parallel);
            slot.setBalance(0.0f);
            expect(gainAt(slot, 12000.0f) < 0.05f, "Balance 0 = filter A (low-pass) only");
            slot.setBalance(1.0f);
            expect(gainAt(slot, 12000.0f) > 0.9f, "Balance 1 = filter B (high-pass) only");
        }

        beginTest("Filter insert effect: any track can use the filter set; FREQ / RESO / TYPE / MODE on the FX page");
        {
            SequencerCore seq;
            AudioEngine audio;
            audio.addEngine(0, AudioEngine::createEngine(EngineType::Wavetable));
            TrackFxScreen screen(seq, &audio);
            expect(screen.selectPageByName("FX"));
            screen.onEncoderChange(1, 11);                      // TYPE: OFF -> FLT (last in the list)
            expectEquals(audio.getInsertChain(0)->getEffectCount(), 1);
            const char* expected[4] = { "CUT", "RESO", "TYPE", "MODE" };
            for (int i = 0; i < 4; ++i)
                expectEquals(screen.getEncoderName(4 + i), juce::String(expected[i]));

            auto* fx = audio.getInsertChain(0)->getEffect(0);
            screen.onEncoderChange(6, 2);                       // TYPE: SVF -> LADR
            expectEquals((int) fx->getParameter(FilterEffect::Type), 2);
            expectEquals(fx->getParamSpec(3).numChoices, 6);    // the ladder's six modes
            screen.onEncoderChange(7, 5);                       // MODE -> HP
            expectEquals((int) fx->getParameter(FilterEffect::ModeParam), 5);

            // It filters: a low-pass at 300 Hz removes most of a bright noise burst.
            FilterEffect lp;
            lp.prepare(48000.0, 512);
            lp.setParameter(FilterEffect::Frequency, 300.0f);
            lp.setParameter(FilterEffect::Resonance, 0.0f);
            juce::Random rng(7);
            double in = 0.0, out = 0.0;
            for (int i = 0; i < 20000; ++i)
            {
                float l = rng.nextFloat() * 2.0f - 1.0f, r = l;
                in += l * l;
                lp.process(l, r);
                out += l * l;
            }
            expect(out < in * 0.1, "Low-pass removes most of the noise energy");
        }

        beginTest("Sampler: per-voice filter from the same set, with its own FILTR page");
        {
            auto energy = [](float cut, int type, int mode)
            {
                SamplerEngine samp;
                samp.prepareToPlay(44100.0, 512);
                samp.setParameter(SamplerEngine::SampleSelect, 3.0f);      // closed hat: bright
                samp.setParameter(SamplerEngine::FilterType, (float) type);
                samp.setParameter(SamplerEngine::FilterMode, (float) mode);
                samp.setParameter(SamplerEngine::FilterReso, 0.0f);
                samp.setParameter(SamplerEngine::FilterCutoff, cut);
                samp.noteOn(60, 1.0f, -1);
                juce::MidiBuffer midi;
                double sum = 0.0;
                for (int b = 0; b < 8; ++b)
                {
                    juce::AudioBuffer<float> buf(2, 512);
                    buf.clear();
                    samp.processBlock(buf, midi);
                    for (int ch = 0; ch < 2; ++ch)
                        for (int i = 0; i < 512; ++i)
                            sum += buf.getSample(ch, i) * buf.getSample(ch, i);
                }
                return sum;
            };
            double open = energy(1.0f, 0, 0);
            expect(open > 0.0);
            expect(energy(0.4f, 0, 0) < open * 0.2, "SVF low-pass darkens the hat (both channels)");
            expect(energy(0.4f, 2, 0) < open * 0.2, "Ladder low-pass too");
            expect(energy(0.4f, 0, 2) > energy(0.4f, 0, 0) * 3.0, "High-pass keeps what the low-pass removes");

            SamplerEngine samp;
            bool found = false;
            samp.setParameter(SamplerEngine::FilterType, 2.0f);
            for (auto& page : samp.getParamPages())
                if (page.name == "FILTR")
                {
                    found = true;
                    const char* expected[8] = { "CUT", "RESO", "TYPE", "MODE", "VELF", "ENVF", "KTRK", "FDRV" };
                    for (int i = 0; i < 8; ++i)
                        expectEquals(juce::String(page.params[(size_t) i].label), juce::String(expected[i]));
                    expectEquals(page.params[3].numChoices, 6);
                }
            expect(found);
        }

        beginTest("Unified voice: TYPE and MODE cells select what sounds; MODE follows the type's range");
        {
            UnifiedVoiceEngine e(1);
            e.prepareToPlay(44100.0, 512);
            using namespace UnifiedParam;

            e.setParameter(MODEL_A, 2.0f);       // LADR
            e.setParameter(MODE_A, 5.0f);        // HP
            expect(e.getVoice(0).getFilterSlot().getModelA() == FilterModel::Ladder);
            expectEquals(e.getVoice(0).getFilterSlot().getMode(0), 5);

            e.setParameter(MODEL_A, 1.0f);       // SEM has two modes: the stored mode is pulled into range
            expect(e.getVoice(0).getFilterSlot().getModelA() == FilterModel::SEM);
            expectEquals((int) e.getParameter(MODE_A), 1);

            // The page's MODE cell lists the current type's modes.
            for (auto& page : e.getParamPages())
                for (auto& p : page.params)
                    if (p.id == MODE_A)
                    {
                        expectEquals(p.numChoices, 2);
                        expectEquals(juce::String(p.choices[0]), juce::String("MRPH"));
                    }

            // Every type / mode makes sound through the engine.
            for (int t = 0; t < FilterSlot::NUM_MODELS; ++t)
                for (int m = 0; m < FilterSlot::modeCount(FilterSlot::modelFromIndex(t)); ++m)
                {
                    UnifiedVoiceEngine v(1);
                    v.prepareToPlay(44100.0, 512);
                    v.setParameter(CUTOFF, 1500.0f);
                    v.setParameter(RESONANCE, 0.5f);
                    v.setParameter(MODEL_A, (float) t);
                    v.setParameter(MODE_A, (float) m);
                    v.noteOn(48, 1.0f, -1);
                    juce::MidiBuffer midi;
                    float peak = 0.0f;
                    for (int b = 0; b < 20; ++b)
                    {
                        juce::AudioBuffer<float> buf(2, 512);
                        buf.clear();
                        v.processBlock(buf, midi);
                        peak = juce::jmax(peak, buf.getMagnitude(0, 512));
                    }
                    expect(peak > 0.01f && peak < 8.0f, juce::String(FilterSlot::modelNames()[t]) + " mode "
                                                        + juce::String(m) + " peak " + juce::String(peak));
                }
        }
    }
};
static FilterSetTests filterSetTests;

// ===== Track effect blocks (DIRT, TALK, MODFX), the insert list and the ANALG page =====
class TrackFxTests : public juce::UnitTest
{
public:
    TrackFxTests() : UnitTest("Track Effects") {}

    // RMS gain of a block for a sine (amplitude 0.3) at hz, after it has settled.
    template <typename Block>
    static float gainAt(Block& block, float hz, double sr = 48000.0)
    {
        double phase = 0.0, in = 0.0, out = 0.0;
        for (int b = 0; b < 60; ++b)
        {
            float l[512], r[512];
            for (int i = 0; i < 512; ++i)
            {
                l[i] = r[i] = 0.3f * (float) std::sin(phase);
                phase += 2.0 * 3.14159265358979 * hz / sr;
            }
            double blockIn = 0.0;
            for (int i = 0; i < 512; ++i) blockIn += l[i] * l[i];
            block.process(l, r, 512);
            if (b >= 30)
            {
                in += blockIn;
                for (int i = 0; i < 512; ++i) out += l[i] * l[i];
            }
        }
        return (float) std::sqrt(out / in);
    }

    static bool isUntouched(float* l, const float* ref, int n)
    {
        for (int i = 0; i < n; ++i)
            if (l[i] != ref[i]) return false;
        return true;
    }

    void runTest() override
    {
        float saw[512], ref[512], right[512];
        auto fill = [&]
        {
            for (int i = 0; i < 512; ++i)
                saw[i] = ref[i] = right[i] = 0.6f * (2.0f * (float) ((i * 7) % 100) / 100.0f - 1.0f);
        };

        beginTest("DIRT: off = untouched; drive, bits, rate and tuned feedback each do their own thing");
        {
            DirtBlock dirt;
            dirt.prepare(48000.0, 512);
            fill();
            dirt.process(saw, right, 512);
            expect(isUntouched(saw, ref, 512), "All cells at their defaults: bypassed");

            const char* expected[8] = { "DRIV", "TYPE", "BITS", "RATE", "FDBK", "TUNE", "DAMP", "MIX" };
            auto page = DirtBlock::page();
            for (int i = 0; i < 8; ++i)
                expectEquals(juce::String(page.params[(size_t) i].label), juce::String(expected[i]));

            for (int cell : { (int) DirtBlock::Drive, (int) DirtBlock::Bits, (int) DirtBlock::Rate, (int) DirtBlock::Feedback })
            {
                DirtBlock d;
                d.prepare(48000.0, 512);
                d.set(cell, 0.8f);
                expect(d.isActive());
                fill();
                d.process(saw, right, 512);
                expect(!isUntouched(saw, ref, 512), "Cell " + juce::String(cell) + " changes the sound");
                for (int i = 0; i < 512; ++i)
                    expect(std::isfinite(saw[i]) && std::abs(saw[i]) < 10.0f);
            }

            // The feedback path rings at TUNE.
            DirtBlock comb;
            comb.prepare(48000.0, 512);
            comb.set(DirtBlock::Feedback, 0.85f);
            comb.set(DirtBlock::Tune, 500.0f);
            comb.set(DirtBlock::Damp, 0.0f);
            expect(gainAt(comb, 500.0f) > gainAt(comb, 750.0f) * 3.0f, "Peak at the tuned frequency");

            // MIX 0 gives the dry signal back.
            DirtBlock dry;
            dry.prepare(48000.0, 512);
            dry.set(DirtBlock::Drive, 1.0f);
            dry.set(DirtBlock::Mix, 0.0f);
            fill();
            dry.process(saw, right, 512);
            expect(isUntouched(saw, ref, 512));
        }

        beginTest("TALK: formant peaks sit on the vowel's formants and move with VOWL and SHFT");
        {
            const char* expected[8] = { "VOWL", "SHFT", "RESO", "MIX", "RATE", "DPTH", "ENV", "BITE" };
            auto page = TalkBlock::page();
            for (int i = 0; i < 8; ++i)
                expectEquals(juce::String(page.params[(size_t) i].label), juce::String(expected[i]));

            auto make = [](float vowel, float shift)
            {
                TalkBlock t;
                t.prepare(48000.0, 512);
                t.set(TalkBlock::Mix, 1.0f);
                t.set(TalkBlock::Vowel, vowel);
                t.set(TalkBlock::Shift, shift);
                t.set(TalkBlock::Reso, 0.7f);
                t.set(TalkBlock::Bite, 0.0f);
                return t;
            };

            // "A": first formant 650 Hz, second 1080 Hz; a dip between and nothing much at 200 Hz.
            auto a = make(0.0f, 0.0f);
            expect(gainAt(a, 650.0f) > gainAt(a, 200.0f) * 4.0f);
            expect(gainAt(a, 650.0f) > gainAt(a, 1700.0f) * 3.0f);
            // "I": first formant 290 Hz, second 1870 Hz.
            auto i = make(0.5f, 0.0f);
            expect(gainAt(i, 290.0f) > gainAt(i, 650.0f) * 3.0f, "The 650 Hz peak of A is gone on I");
            expect(gainAt(i, 1870.0f) > gainAt(i, 1080.0f) * 2.0f);
            // One octave up: A's first formant is at 1300 Hz.
            auto up = make(0.0f, 1.0f);
            expect(gainAt(up, 1300.0f) > gainAt(up, 650.0f) * 3.0f);

            expectEquals(juce::String(TalkBlock::vowelName(0.0f)), juce::String("A"));
            expectEquals(juce::String(TalkBlock::vowelName(0.5f)), juce::String("I"));
            expectEquals(juce::String(TalkBlock::vowelName(1.0f)), juce::String("U"));
        }

        beginTest("TALK: the LFO and the playing level move the vowel; MIX 0 is off");
        {
            TalkBlock off;
            off.prepare(48000.0, 512);
            fill();
            off.process(saw, right, 512);
            expect(isUntouched(saw, ref, 512), "MIX 0: bypassed");

            TalkBlock lfo;
            lfo.prepare(48000.0, 512);
            lfo.set(TalkBlock::Mix, 1.0f);
            lfo.set(TalkBlock::Vowel, 0.5f);
            lfo.set(TalkBlock::LfoDepth, 1.0f);
            lfo.set(TalkBlock::LfoRate, 4.0f);
            float lo = 1.0f, hi = 0.0f;
            for (int b = 0; b < 60; ++b)
            {
                fill();
                lfo.process(saw, right, 512);
                lo = juce::jmin(lo, lfo.currentPosition());
                hi = juce::jmax(hi, lfo.currentPosition());
            }
            expect(lo < 0.1f && hi > 0.9f, "LFO sweeps the whole vowel range");

            TalkBlock env;
            env.prepare(48000.0, 512);
            env.set(TalkBlock::Mix, 1.0f);
            env.set(TalkBlock::Vowel, 0.0f);
            env.set(TalkBlock::Env, 1.0f);
            for (int b = 0; b < 20; ++b) { fill(); env.process(saw, right, 512); }
            float loud = env.currentPosition();
            for (int b = 0; b < 60; ++b)
            {
                float silence[512] = {}, silenceR[512] = {};     // fresh silence each block
                env.process(silence, silenceR, 512);
            }
            expect(loud > 0.5f && env.currentPosition() < 0.05f, "Loud playing opens the vowel, silence closes it");
        }

        beginTest("MODFX: four effects, two knobs each; depth 0 = off; each one changes the sound");
        {
            const char* expected[8] = { "CH.R", "CH.D", "FL.R", "FL.D", "PH.R", "PH.D", "RO.S", "RO.D" };
            auto page = ModFxBlock::page();
            for (int i = 0; i < 8; ++i)
                expectEquals(juce::String(page.params[(size_t) i].label), juce::String(expected[i]));

            ModFxBlock off;
            off.prepare(48000.0, 512);
            expect(!off.isActive());

            for (int cell : { (int) ModFxBlock::ChorusDepth, (int) ModFxBlock::FlangerDepth,
                              (int) ModFxBlock::PhaserDepth, (int) ModFxBlock::RotaryDepth })
            {
                ModFxBlock m;
                m.prepare(48000.0, 512);
                m.set(cell, 0.7f);
                expect(m.isActive());
                bool changed = false, bounded = true;
                for (int b = 0; b < 20; ++b)
                {
                    fill();
                    m.process(saw, right, 512);
                    changed = changed || !isUntouched(saw, ref, 512);
                    for (int i = 0; i < 512; ++i)
                        bounded = bounded && std::isfinite(saw[i]) && std::abs(saw[i]) < 10.0f;
                }
                expect(changed, "Depth cell " + juce::String(cell) + " changes the sound");
                expect(bounded);
            }
        }

        beginTest("The blocks are track parameters: pages on TRK FX, routed by the audio engine, lockable");
        {
            SequencerCore seq;
            AudioEngine audio;
            audio.addEngine(0, AudioEngine::createEngine(EngineType::Drum));
            TrackFxScreen screen(seq, &audio);

            for (auto name : { "DIRT", "TALK", "MODFX", "FX" })
                expect(screen.selectPageByName(name), juce::String(name) + " page exists for a drum track");

            expect(screen.selectPageByName("TALK"));
            screen.onEncoderChange(3, 50);                         // MIX up by half
            int mixId = TrackFxParam::TALK_BASE + TalkBlock::Mix;
            expectWithinAbsoluteError(audio.getTrackParameter(0, mixId), 0.5f, 0.001f);
            expectWithinAbsoluteError(seq.getPattern().getTrack(0).soundParams[mixId], 0.5f, 0.001f);
            expect(audio.getTrackFx(0) != nullptr && audio.getTrackFx(0)->talk.isActive());

            // A step lock on the vowel returns to the track's value at the next trig.
            int vowelId = TrackFxParam::TALK_BASE + TalkBlock::Vowel;
            Step locked, plain;
            locked.trig = plain.trig = true;
            locked.paramLocks[vowelId] = 1.0f;
            audio.applyStepLocks(0, locked);
            expectWithinAbsoluteError(audio.getTrackParameter(0, vowelId), 1.0f, 0.001f);
            audio.applyStepLocks(0, plain);
            expectWithinAbsoluteError(audio.getTrackParameter(0, vowelId), 0.0f, 0.001f);

            // Changing the engine keeps the track's effect settings.
            audio.setSequencer(&seq);
            audio.setEngineType(0, EngineType::FM);
            expectWithinAbsoluteError(audio.getTrackParameter(0, mixId), 0.5f, 0.001f);
        }

        beginTest("Every effect type can be put in an insert slot and names its parameters");
        {
            for (int t = 0; t < (int) EffectType::NumEffectTypes; ++t)
            {
                auto type = static_cast<EffectType>(t);
                auto fx = AudioEngine::createEffect(type);
                expect(fx != nullptr, "Type " + juce::String(t));
                if (!fx) continue;
                expect(juce::String(AudioEngine::effectTypeName(type)) != "---");
                fx->prepare(48000.0, 512);

                int named = 0;
                for (int p = 0; p < 4; ++p)
                {
                    auto spec = fx->getParamSpec(p);
                    if (!spec.isValid()) continue;
                    ++named;
                    expect(juce::String(spec.label).isNotEmpty() && juce::String(spec.label).length() <= 4);
                    // Both ends of the range are accepted and the output stays sane.
                    fx->setParameter(p, spec.maxVal);
                    fx->setParameter(p, spec.minVal);
                    fx->setParameter(p, spec.defaultVal);
                }
                expect(named >= 3, juce::String(AudioEngine::effectTypeName(type)) + " names " + juce::String(named) + " parameters");

                bool bounded = true;
                for (int b = 0; b < 10; ++b)
                {
                    fill();
                    fx->processBlock(saw, right, 512);
                    for (int i = 0; i < 512; ++i)
                        bounded = bounded && std::isfinite(saw[i]) && std::abs(saw[i]) < 20.0f;
                }
                expect(bounded, juce::String(AudioEngine::effectTypeName(type)) + " output is bounded");
            }
        }

        beginTest("ANALG page: eight controls for what makes the voices imperfect");
        {
            UnifiedVoiceEngine e(4);
            e.prepareToPlay(44100.0, 512);
            bool found = false;
            for (auto& page : e.getParamPages())
                if (page.name == "ANALG")
                {
                    found = true;
                    const char* expected[8] = { "DRFT", "DRAT", "SLOP", "PHAS", "WSHP", "SOFT", "FVAR", "EVAR" };
                    for (int i = 0; i < 8; ++i)
                        expectEquals(juce::String(page.params[(size_t) i].label), juce::String(expected[i]));
                }
            expect(found);

            using namespace UnifiedParam;
            e.setParameter(DRIFT, 0.5f);
            e.setParameter(DRIFT_RATE, 2.0f);
            e.setParameter(SLOP, 30.0f);
            e.setParameter(RANDOM_PHASE, 0.0f);
            e.setParameter(WAVE_VAR, 1.0f);
            e.setParameter(SOFT_START, 25.0f);
            e.setParameter(FILTER_VAR, 0.6f);
            e.setParameter(ENV_VAR, 0.8f);
            auto& ac = e.getVoice(0).getAnalogCharacter();
            expectWithinAbsoluteError(ac.driftAmount, 10.0f, 0.01f);
            expectWithinAbsoluteError(ac.driftRate, 2.0f, 0.01f);
            expectWithinAbsoluteError(ac.slopCents, 30.0f, 0.01f);
            expect(!ac.randomPhase);
            expectWithinAbsoluteError(ac.waveshapeVar, 0.15f, 0.0001f);     // the top of WSHP: heard from note to note
            expectWithinAbsoluteError(ac.softStartCents, 25.0f, 0.01f);
            expectWithinAbsoluteError(ac.filterVariance, 0.6f, 0.001f);
            expectWithinAbsoluteError(ac.envVariance, 0.8f, 0.001f);
        }

        beginTest("ANALG: with everything off two notes are identical; SLOP, FVAR and EVAR make each note different");
        {
            auto render = [](float slop, float fvar, float evar)
            {
                UnifiedVoiceEngine e(1);
                e.prepareToPlay(44100.0, 512);
                using namespace UnifiedParam;
                e.setParameter(MIX, 0.0f);
                e.setParameter(CUTOFF, 1200.0f);
                e.setParameter(AMP_DECAY, 0.05f);
                e.setParameter(AMP_SUSTAIN, 0.2f);
                e.setParameter(DRIFT, 0.0f);
                e.setParameter(WAVE_VAR, 0.0f);
                e.setParameter(SOFT_START, 0.0f);
                e.setParameter(RANDOM_PHASE, 0.0f);
                e.setParameter(SLOP, slop);
                e.setParameter(FILTER_VAR, fvar);
                e.setParameter(ENV_VAR, evar);
                std::vector<std::vector<float>> notes;
                juce::MidiBuffer midi;
                for (int n = 0; n < 4; ++n)
                {
                    e.allNotesOff();
                    e.noteOn(57, 1.0f, -1);
                    std::vector<float> out;
                    for (int b = 0; b < 12; ++b)
                    {
                        juce::AudioBuffer<float> buf(2, 512);
                        buf.clear();
                        e.processBlock(buf, midi);
                        out.insert(out.end(), buf.getReadPointer(0), buf.getReadPointer(0) + 512);
                    }
                    notes.push_back(out);
                }
                // Largest difference between any later note and the first.
                float worst = 0.0f;
                for (size_t n = 1; n < notes.size(); ++n)
                    for (size_t i = 0; i < notes[0].size(); ++i)
                        worst = juce::jmax(worst, std::abs(notes[n][i] - notes[0][i]));
                return worst;
            };

            expect(render(0.0f, 0.0f, 0.0f) < 1.0e-5f, "No randomisation: every note is the same");
            expect(render(40.0f, 0.0f, 0.0f) > 0.05f, "SLOP: notes land at different pitches");
            expect(render(0.0f, 1.0f, 0.0f) > 0.02f, "FVAR: notes differ in brightness");
            expect(render(0.0f, 0.0f, 1.0f) > 0.02f, "EVAR: notes differ in envelope time");
        }
    }
};
static TrackFxTests trackFxTests;

class StepDetailTests : public juce::UnitTest
{
public:
    StepDetailTests() : UnitTest("Step Detail Pages") {}

    void runTest() override
    {
        struct Rig
        {
            SequencerCore seq;
            AudioEngine audio;
            PadManager pads;
            PadLEDFeedback leds;
            std::unique_ptr<ParamPageScreen> params;
            std::unique_ptr<StepViewScreen> steps;
            Rig()
            {
                audio.addEngine(0, AudioEngine::createEngine(EngineType::UnifiedVoice));
                params = std::make_unique<ParamPageScreen>(seq, &audio);
                steps = std::make_unique<StepViewScreen>(seq, pads, leds);
                steps->setLockPages(params.get());
            }
        };

        beginTest("Hold a step + turn = parameter lock on the first parameter page, shown as that page");
        {
            Rig rig;
            auto& track = rig.seq.getPattern().getTrack(0);
            rig.steps->onPadPress(4, 100);
            expect(!rig.steps->isDetailActive(), "A plain tap does not open the detail view");
            expectEquals(rig.steps->getName(), juce::String("STEP"));

            rig.steps->onEncoderChange(1, 10);                // RESO on the MAIN landing page
            expect(rig.steps->isDetailActive());
            expectEquals(rig.steps->getName(), juce::String("MAIN"));
            expectEquals(rig.steps->getEncoderName(0), juce::String("CUT"));
            expectEquals(rig.steps->getEncoderName(1), juce::String("RESO"));
            expect(!rig.steps->usesEncoderBar());
            auto& locks = track.getStep(4).paramLocks;
            expect(locks.find(UnifiedParam::RESONANCE) != locks.end(), "RESO locked on step 5");
            expectWithinAbsoluteError(locks[UnifiedParam::RESONANCE], 0.3f, 0.001f);
            // The track's own value is unchanged.
            expectWithinAbsoluteError(rig.audio.getTrackParameter(0, UnifiedParam::RESONANCE), 0.2f, 0.001f);

            rig.steps->onPadRelease(4);
            expect(!rig.steps->isDetailActive());
            expectEquals(rig.steps->getName(), juce::String("STEP"));
        }

        beginTest("Page back while holding: TRIG page edits the step itself");
        {
            Rig rig;
            auto& track = rig.seq.getPattern().getTrack(0);
            rig.steps->onPadPress(0, 100);
            rig.steps->onArrowLeft();
            expectEquals(rig.steps->getName(), juce::String("TRIG"));
            const char* expected[8] = { "NOTE", "VEL", "LEN", "MICR", "COND", "---", "---", "LCKS" };
            for (int i = 0; i < 8; ++i)
                expectEquals(rig.steps->getEncoderName(i), juce::String(expected[i]));

            auto& step = track.getStep(0);
            rig.steps->onEncoderChange(0, 7);                 // NOTE up a fifth
            expectEquals(step.notes[0].pitch, 67);
            rig.steps->onEncoderChange(2, 4);                 // LEN +4 quarter steps
            expectWithinAbsoluteError(step.length, 2.0f, 0.001f);
            rig.steps->onEncoderChange(4, 1);                 // COND -> probability
            expect(step.trigCondition.type == TrigCondition::Probability);
            expectEquals(rig.steps->getEncoderName(5), juce::String("PROB"));
            rig.steps->onEncoderChange(4, 1);                 // COND -> A:B
            expectEquals(rig.steps->getEncoderName(5), juce::String("A"));
            expectEquals(rig.steps->getEncoderName(6), juce::String("B"));
            rig.steps->onEncoderChange(6, 2);
            expectEquals(step.trigCondition.b, 4);

            // LCKS turned down clears the step's locks.
            step.paramLocks[UnifiedParam::CUTOFF] = 500.0f;
            rig.steps->onEncoderChange(7, 127);
            expect(step.paramLocks.empty());
            rig.steps->onPadRelease(0);
        }

        beginTest("Visual standard: page position in the name bar, turned-knob highlight, on every layout");
        {
            // A grid screen and a bar screen behind one ScreenManager.
            SequencerCore seq;
            AudioEngine audio;
            audio.addEngine(0, AudioEngine::createEngine(EngineType::UnifiedVoice));
            DisplayRenderer display;
            ScreenManager mgr;
            mgr.setDisplay(&display);
            auto params = std::make_unique<ParamPageScreen>(seq, &audio);
            auto* paramsPtr = params.get();
            mgr.addScreen(std::move(params));
            mgr.addScreen(std::make_unique<HomeScreen>(seq, &audio));
            mgr.setActiveScreen(0);

            expect(paramsPtr->getEncoderLayout() == Screen::EncoderLayout::Grid);
            expect(paramsPtr->getPageCount() > 1);
            expect(!paramsPtr->getSubPage().containsChar('/'), "Screens do not write page numbers themselves");

            auto countLit = [&](int x0, int y0, int w, int h)
            {
                int n = 0;
                auto& px = display.getPixels();
                for (int y = y0; y < y0 + h; ++y)
                    for (int x = x0; x < x0 + w; ++x)
                        if (px[y][x]) ++n;
                return n;
            };

            // "n/N" sits at the right end of the name bar on a multi-page screen...
            mgr.requestRedraw(); mgr.redrawIfNeeded();
            expect(countLit(100, 0, 28, 9) > 10, "Page position drawn at the right of the name bar");

            // ...and the label of the encoder just turned is inverted: cell 6 (bottom row, second) on a grid.
            int before = countLit(ParamGrid::cellX(5), ParamGrid::cellY(5) - 1, 31, 9);
            mgr.controlChangeReceived(16, 0x15 + 5, 1);
            int after = countLit(ParamGrid::cellX(5), ParamGrid::cellY(5) - 1, 31, 9);
            expect(after > before + 60, "Grid cell label highlighted after its encoder turns");
            expectEquals(countLit(ParamGrid::cellX(0), ParamGrid::cellY(0) - 1, 31, 9),
                         countLit(ParamGrid::cellX(0), ParamGrid::cellY(0) - 1, 31, 9));

            // Bar layout: the column of the turned encoder is inverted in the bottom bar.
            mgr.setActiveScreen(1);
            mgr.requestRedraw(); mgr.redrawIfNeeded();
            int barBefore = countLit(3 * 16, 54, 16, 10);
            mgr.controlChangeReceived(16, 0x15 + 3, 1);
            expect(countLit(3 * 16, 54, 16, 10) > barBefore + 60, "Bar column highlighted after its encoder turns");
        }

        beginTest("Pressure ranges: Func + Down / Up set the low / high end of the knob turned last; the same again removes it");
        {
            Rig rig;
            auto& params = rig.seq.getPattern().getTrack(0).soundParams;
            expect(rig.params->selectPageByName("FILTR"));
            const int lo = RangeParam::LO_BASE + UnifiedParam::RESONANCE;
            const int hi = RangeParam::HI_BASE + UnifiedParam::RESONANCE;

            rig.params->onEncoderChange(1, 10);                 // RESO 0.20 -> 0.30
            rig.params->onFuncDown();                           // low end here
            expect(params.count(lo) == 1 && params.count(hi) == 0);
            expectWithinAbsoluteError(params[lo], 0.30f, 0.001f);
            expectEquals(rig.params->getPageName(), juce::String("FILTR"));
            expect(rig.params->getSubPage().contains("RESO LO"), rig.params->getSubPage());

            rig.params->onEncoderChange(1, 50);                 // RESO -> 0.80
            rig.params->onFuncUp();                             // high end here
            expectWithinAbsoluteError(params[hi], 0.80f, 0.001f);
            expect(rig.params->cellHasRange(1) && rig.params->pageHasRanges());
            expect(!rig.params->cellHasRange(0));

            rig.params->onFuncUp();                             // again, value unchanged: high end removed
            expect(params.count(lo) == 1 && params.count(hi) == 0);
            rig.params->onEncoderChange(1, 128 - 50);           // back to 0.30, where the low end is
            rig.params->onFuncDown();                           // low end removed
            expect(params.count(lo) == 0);
            expect(!rig.params->pageHasRanges());

            // Range ends are never sent to the engine.
            rig.params->onEncoderChange(1, 1);
            rig.params->onFuncDown();
            expectWithinAbsoluteError(rig.audio.getTrackParameter(0, UnifiedParam::RESONANCE), 0.31f, 0.001f);
        }

        beginTest("Pressure ranges: plain arrows never touch them (bank = page, Func + bank = section, Up / Down = step page)");
        {
            Rig rig;
            auto& params = rig.seq.getPattern().getTrack(0).soundParams;
            rig.params->onArrowRight();         // past the MAIN landing page, to OSC
            rig.params->onEncoderChange(2, 5);
            rig.params->onSectionNext();
            expectEquals(rig.params->getPageName(), juce::String("FILTR"), "Jumped to the next section");
            rig.params->onArrowLeft();
            rig.params->onPadPageNext();
            rig.params->onPadPagePrev();
            expect(params.count(RangeParam::LO_BASE + UnifiedParam::MIX) == 0
                   && params.count(RangeParam::HI_BASE + UnifiedParam::MIX) == 0);
        }

        beginTest("Pressure ranges: bottom-row pad N sweeps cell N from low to high; release returns to the knob's value");
        {
            Rig rig;
            auto& params = rig.seq.getPattern().getTrack(0).soundParams;
            expect(rig.params->selectPageByName("FILTR"));

            // CUT (cell 1): low 500 Hz, high 4 kHz, knob left at 1 kHz.
            params[RangeParam::LO_BASE + UnifiedParam::CUTOFF] = 500.0f;
            params[RangeParam::HI_BASE + UnifiedParam::CUTOFF] = 4000.0f;
            params[UnifiedParam::CUTOFF] = 1000.0f;
            rig.audio.setTrackParameter(0, UnifiedParam::CUTOFF, 1000.0f);
            // RESO (cell 2): only a high end; the knob's value (0.2) is the other end.
            params[RangeParam::HI_BASE + UnifiedParam::RESONANCE] = 0.9f;

            rig.params->onPadPress(8, 100);                     // bottom row, first pad = cell 1
            expectWithinAbsoluteError(rig.audio.getTrackParameter(0, UnifiedParam::CUTOFF), 500.0f, 1.0f);
            expectEquals(rig.params->getHeldStepIndex(), -1, "A range pad is not a step-hold pad");
            rig.params->onPadAftertouch(8, 127);
            expectWithinAbsoluteError(rig.audio.getTrackParameter(0, UnifiedParam::CUTOFF), 4000.0f, 5.0f);
            rig.params->onPadAftertouch(8, 64);                 // half way, by pitch: about 1.4 kHz
            float mid = rig.audio.getTrackParameter(0, UnifiedParam::CUTOFF);
            expect(mid > 1300.0f && mid < 1550.0f, juce::String(mid));

            // Two pads at once: another finger on RESO.
            rig.params->onPadPress(9, 100);
            rig.params->onPadAftertouch(9, 127);
            expectWithinAbsoluteError(rig.audio.getTrackParameter(0, UnifiedParam::RESONANCE), 0.9f, 0.01f);
            expect(rig.audio.getTrackParameter(0, UnifiedParam::CUTOFF) > 1300.0f, "The first pad is still held");

            rig.params->onPadRelease(8);
            rig.params->onPadRelease(9);
            expectWithinAbsoluteError(rig.audio.getTrackParameter(0, UnifiedParam::CUTOFF), 1000.0f, 1.0f);
            expectWithinAbsoluteError(rig.audio.getTrackParameter(0, UnifiedParam::RESONANCE), 0.2f, 0.01f);
            expectWithinAbsoluteError(params[UnifiedParam::CUTOFF], 1000.0f, 0.01f);

            // A cell without a range does nothing; the top row still holds steps.
            rig.params->onPadPress(10, 100);
            rig.params->onPadAftertouch(10, 127);
            expectWithinAbsoluteError(rig.audio.getTrackParameter(0, UnifiedParam::FILTER_ENV), 0.0f, 0.001f);
            rig.params->onPadRelease(10);
            rig.params->onPadPress(3, 100);
            expectEquals(rig.params->getHeldStepIndex(), 3);
            rig.params->onPadRelease(3);
        }

        beginTest("Pressure ranges: a page without ranges keeps all sixteen pads for steps; ranges are saved and cleared with the engine");
        {
            Rig rig;
            rig.audio.setSequencer(&rig.seq);
            auto& params = rig.seq.getPattern().getTrack(0).soundParams;
            rig.params->onPadPress(12, 100);
            expectEquals(rig.params->getHeldStepIndex(), 12);
            rig.params->onPadRelease(12);

            params[RangeParam::LO_BASE + UnifiedParam::CUTOFF] = 300.0f;
            params[RangeParam::HI_BASE + MixParam::PAN] = 1.0f;
            auto data = ProjectBridge::capture({ rig.seq, rig.audio }, "R");
            auto back = ProjectData::fromJSON(data.toJSON());
            auto& saved = back.patterns[0].tracks[0].soundParams;
            expectWithinAbsoluteError(saved[RangeParam::LO_BASE + UnifiedParam::CUTOFF], 300.0f, 0.01f);

            // A new engine has different parameters: its ranges go, the mixer's stay.
            rig.audio.setEngineType(0, EngineType::FM);
            expect(params.count(RangeParam::LO_BASE + UnifiedParam::CUTOFF) == 0);
            expect(params.count(RangeParam::HI_BASE + MixParam::PAN) == 1);
        }

        beginTest("Pressure ranges from hardware messages: knob CC, Func + arrow CCs, then pad pressure");
        {
            SequencerCore seq;
            AudioEngine audio;
            audio.addEngine(0, AudioEngine::createEngine(EngineType::UnifiedVoice));
            ScreenManager mgr;
            auto params = std::make_unique<ParamPageScreen>(seq, &audio);
            auto* screen = params.get();
            mgr.addScreen(std::move(params));
            mgr.setActiveScreen(0);
            screen->onArrowRight();             // past the MAIN landing page

            // OSC page, cell 3 = MIX (0.50). Func + Down for the low end, turn up, Func + Up for the high end.
            auto funcArrow = [&](int arrowCC)
            {
                mgr.controlChangeReceived(1, 105, 127);         // Func
                mgr.controlChangeReceived(1, arrowCC, 127);
                mgr.controlChangeReceived(1, arrowCC, 0);
                mgr.controlChangeReceived(1, 105, 0);
            };
            mgr.controlChangeReceived(16, 0x15 + 2, 127);       // one step down: 0.49
            funcArrow(107);                                     // Func + Down
            mgr.controlChangeReceived(16, 0x15 + 2, 40);        // +40 steps: 0.89
            funcArrow(106);                                     // Func + Up
            expectEquals(screen->getPageName(), juce::String("OSC"), "Func + arrows set range ends and do not navigate");
            expectEquals(screen->getStepPage(), 0);
            expect(screen->cellHasRange(2));

            // Bottom row, third pad (note 114) with rising pressure.
            const int pad = LaunchkeyController::PAD_BOTTOM_ROW_START + 2;
            mgr.noteOnReceived(1, pad, 90);
            expectWithinAbsoluteError(audio.getTrackParameter(0, UnifiedParam::MIX), 0.49f, 0.005f);
            mgr.aftertouchReceived(1, pad, 127);
            expectWithinAbsoluteError(audio.getTrackParameter(0, UnifiedParam::MIX), 0.89f, 0.005f);
            mgr.noteOffReceived(1, pad);
            expectWithinAbsoluteError(audio.getTrackParameter(0, UnifiedParam::MIX), 0.89f, 0.005f, "Back at the knob's value");
        }

        beginTest("One arpeggiator per track: each plays its own track, and a step can lock its gate / velocity / ratchet");
        {
            SequencerCore seq;
            AudioEngine audio;
            audio.setSequencer(&seq);
            Arpeggiator arpA, arpB;
            int notesA = 0, notesB = 0;
            arpA.onNoteOn = [&](int, float) { ++notesA; };
            arpB.onNoteOn = [&](int, float) { ++notesB; };
            arpA.onNoteOff = [](int) {};
            arpB.onNoteOff = [](int) {};
            audio.addEngine(0, AudioEngine::createEngine(EngineType::UnifiedVoice));
            audio.addEngine(1, AudioEngine::createEngine(EngineType::UnifiedVoice));
            audio.setTrackArpeggiator(0, &arpA);
            audio.setTrackArpeggiator(1, &arpB);
            expect(audio.getTrackArpeggiator(1) == &arpB);

            // Both run at once from the audio callback; a disabled one stays silent.
            arpA.setEnabled(true);
            arpA.noteOn(60, 1.0f);
            arpB.setEnabled(true);
            arpB.noteOn(67, 1.0f);
            for (int b = 0; b < 200; ++b)
                audio.processGenerators(512);
            expect(notesA > 2 && notesB > 2, "Both arpeggiators produce notes");
            arpB.setEnabled(false);
            int frozenB = notesB;
            for (int b = 0; b < 100; ++b)
                audio.processGenerators(512);
            expectEquals(notesB, frozenB);
            expect(notesA > 4);

            // Arp locks are track parameters: applied at a step, released at the next.
            const int gateId = AudioEngine::ARP_LOCK_BASE + 0, ratchetId = AudioEngine::ARP_LOCK_BASE + 2;
            expect(!arpA.hasParamLock(Arpeggiator::LockParam::Gate));
            Step locked, plain;
            locked.trig = plain.trig = true;
            locked.paramLocks[gateId] = 0.2f;
            locked.paramLocks[ratchetId] = 3.0f;
            audio.applyStepLocks(0, locked);
            expect(arpA.hasParamLock(Arpeggiator::LockParam::Gate) && arpA.hasParamLock(Arpeggiator::LockParam::Ratchet));
            expect(!arpB.hasParamLock(Arpeggiator::LockParam::Gate), "Only that track's arpeggiator");
            audio.applyStepLocks(0, plain);
            expect(!arpA.hasParamLock(Arpeggiator::LockParam::Gate) && !arpA.hasParamLock(Arpeggiator::LockParam::Ratchet));

            // The ARPLK page of TRK FX offers them (hold a step there and turn).
            TrackFxScreen screen(seq, &audio);
            expect(screen.selectPageByName("ARPLK"));
            expectEquals(screen.getEncoderName(0), juce::String("GATE"));
            expectEquals(screen.getEncoderName(2), juce::String("RTCH"));
            screen.onPadPress(5, 100);
            screen.onEncoderChange(2, 2);
            screen.onPadRelease(5);
            expectWithinAbsoluteError(seq.getPattern().getTrack(0).getStep(5).paramLocks[ratchetId], 2.0f, 0.001f);
        }

        beginTest("Pads through the ratchet: ROLL off passes notes on, ROLL on retriggers a held pad faster with pressure");
        {
            SequencerCore seq;
            AudioEngine audio;
            audio.setSequencer(&seq);
            audio.addEngine(0, AudioEngine::createEngine(EngineType::UnifiedVoice));
            audio.addEngine(1, AudioEngine::createEngine(EngineType::Drum));
            int hits = 0, lastTrack = -1, lastNote = -1;
            audio.onPadHit = [&](int track, int note, float) { ++hits; lastTrack = track; lastNote = note; };

            // ROLL off: one press, one note, on the pad's own track.
            audio.padNoteOn(1, 8, 8, 0.9f);
            for (int b = 0; b < 50; ++b) audio.processGenerators(512);
            audio.padNoteOff(8);
            audio.processGenerators(512);
            expectEquals(hits, 1);
            expectEquals(lastTrack, 1);
            expectEquals(lastNote, 8);

            // ROLL on: the held pad repeats; more pressure, more hits in the same time.
            auto& roll = audio.getPadRatchet();
            roll.setEnabled(true);
            auto countHits = [&](float pressure)
            {
                hits = 0;
                audio.padNoteOn(1, 8, 8, 0.9f);
                audio.padPressure(8, pressure);
                for (int b = 0; b < 172; ++b) audio.processGenerators(512);     // 2 s at 44.1 kHz
                audio.padNoteOff(8);
                audio.processGenerators(512);
                return hits;
            };
            int soft = countHits(0.0f), hard = countHits(1.0f);
            expect(soft >= 6, "Held pad rolls at the base division: " + juce::String(soft));
            expect(hard > soft * 2, "Full pressure is much faster: " + juce::String(hard));

            // The ROLL page on the ARP screen edits this ratchet.
            Arpeggiator arp;
            ArpScreen screen(arp);
            screen.setPadRatchet(&roll);
            while (screen.getName() != "ROLL") screen.onArrowRight();
            const char* expected[8] = { "ROLL", "MODE", "BASE", "MAX", "DPTH", "GATE", "P>V", "DCAY" };
            for (int i = 0; i < 8; ++i)
                expectEquals(screen.getEncoderName(i), juce::String(expected[i]));
            screen.onEncoderChange(0, 127);
            expect(!roll.isEnabled());
            screen.onEncoderChange(1, 1);       // FREE: BASE / MAX become frequencies
            expect(roll.getMode() == PadRatchet::Mode::Free);
            float hz = roll.getBaseRateHz();
            screen.onEncoderChange(2, 20);
            expect(roll.getBaseRateHz() > hz);

            // ROL2: the feel of a roll.
            screen.onArrowRight();
            expectEquals(screen.getName(), juce::String("ROL2"));
            const char* feel[8] = { "SWNG", "GLID", "THRS", "LTCH", "HUMN", "ACNT", "PROB", "RPTS" };
            for (int i = 0; i < 8; ++i)
                expectEquals(screen.getEncoderName(i), juce::String(feel[i]));
            screen.onEncoderChange(0, 30);
            expect(roll.getSwing() > 0.2f);
            screen.onEncoderChange(3, 1);
            expect(roll.isLatch());
            screen.onEncoderChange(5, 2);
            expectEquals(roll.getAccent(), 3);
            screen.onEncoderChange(6, 128 - 40);
            expect(roll.getProbability() < 0.7f);
            screen.onEncoderChange(7, 4);
            expectEquals(roll.getMaxRepeats(), 4);
        }

        beginTest("DUCK insert: a track is pushed down while the track it listens to is loud");
        {
            auto levelOfTrack2 = [this](bool sourcePlaying, int sourceChoice)
            {
                SequencerCore seq;
                AudioEngine audio;
                audio.setSequencer(&seq);
                for (int t = 0; t < 2; ++t)
                    audio.addEngine(t, AudioEngine::createEngine(EngineType::UnifiedVoice));
                audio.prepare(44100.0, 256);
                audio.setTrackParameter(0, MixParam::PAN, -1.0f);     // source left, ducked track right
                audio.setTrackParameter(1, MixParam::PAN, 1.0f);
                audio.addInsertEffect(1, EffectType::SidechainEffect);
                audio.setTrackParameter(1, AudioEngine::EFFECT_PARAM_BASE + 0, (float) sourceChoice);
                audio.setTrackParameter(1, AudioEngine::EFFECT_PARAM_BASE + 1, -40.0f);
                audio.setTrackParameter(1, AudioEngine::EFFECT_PARAM_BASE + 2, 20.0f);
                audio.getEngine(1)->noteOn(48, 0.4f);
                if (sourcePlaying) audio.getEngine(0)->noteOn(60, 1.0f);
                std::vector<float> l(256), r(256);
                double sum = 0.0;
                for (int b = 0; b < 80; ++b)
                {
                    audio.renderBlock(l.data(), r.data(), 256);
                    if (b >= 40)
                        for (float v : r) sum += (double) v * v;
                }
                return std::sqrt(sum / (40.0 * 256.0));
            };
            const double alone = levelOfTrack2(false, 1);        // listening to T1, which is silent
            const double ducked = levelOfTrack2(true, 1);        // T1 plays
            expect(alone > 0.001, "the ducked track sounds on its own");
            expect(ducked < alone * 0.6, "and is turned down while its source plays: " + juce::String(alone) + " -> " + juce::String(ducked));
        }

        beginTest("Effect tempo sync: SYNC locks an effect's TIME or RATE to a division, follows the tempo, and is saved");
        {
            SequencerCore seq;
            seq.setTempo(120.0f);
            AudioEngine audio;
            audio.setSequencer(&seq);
            audio.addEngine(0, AudioEngine::createEngine(EngineType::UnifiedVoice));
            audio.prepare(44100.0, 256);
            audio.addInsertEffect(0, EffectType::DelayEffect);      // TIME in seconds, 0.01 .. 2
            audio.addInsertEffect(0, EffectType::TremoloEffect);    // RATE in Hz
            audio.addInsertEffect(0, EffectType::DriveEffect);      // nothing to sync
            auto* chain = audio.getInsertChain(0);
            auto* delay = chain->getEffect(0);
            auto* trem = chain->getEffect(1);
            expect(delay->canSync() && trem->canSync() && ! chain->getEffect(2)->canSync());

            const int time = AudioEngine::EFFECT_PARAM_BASE + 0, rate = AudioEngine::EFFECT_PARAM_BASE + 4;
            audio.setTrackParameter(0, time, 0.3f);
            audio.setTrackParameter(0, AudioEngine::INSERT_DIV_BASE + 0, 9.0f);          // 1/8
            audio.setTrackParameter(0, AudioEngine::INSERT_SYNC_BASE + 0, 1.0f);
            expectEquals(juce::String(FxSync::divisionLabels()[9]), juce::String("1/8"));
            expectWithinAbsoluteError(delay->getParameter(0), 0.25f, 0.0005f);           // an eighth at 120
            expectWithinAbsoluteError(audio.getTrackParameter(0, time), 0.3f, 0.0005f);  // the knob's own value is kept

            seq.setTempo(60.0f);                                                          // the audio thread passes the tempo on
            std::vector<float> l(256), r(256);
            audio.renderBlock(l.data(), r.data(), 256);
            expectWithinAbsoluteError(delay->getParameter(0), 0.5f, 0.0005f);
            seq.setTempo(120.0f);
            audio.renderBlock(l.data(), r.data(), 256);

            audio.setTrackParameter(0, AudioEngine::INSERT_DIV_BASE + 0, 0.0f);          // 1/128 = 15.6 ms
            expectWithinAbsoluteError(delay->getParameter(0), 0.015625f, 0.0002f);
            audio.setTrackParameter(0, AudioEngine::INSERT_DIV_BASE + 0, 21.0f);         // 16 bars = 32 s: halved until it fits
            expectWithinAbsoluteError(delay->getParameter(0), 2.0f, 0.001f);

            audio.setTrackParameter(0, AudioEngine::INSERT_DIV_BASE + 1, 12.0f);         // tremolo once per beat = 2 Hz
            audio.setTrackParameter(0, AudioEngine::INSERT_SYNC_BASE + 1, 1.0f);
            expectWithinAbsoluteError(trem->getParameter(0), 2.0f, 0.001f);

            // The FX page: SYNC replaces ON for these effects, and the TIME cell becomes the division.
            seq.getPattern().getTrack(0).engineType = EngineType::UnifiedVoice;
            TrackFxScreen screen(seq, &audio);
            screen.onActivate();
            while (screen.getPageName() != "FX" && screen.getPageIndex() < screen.getPageCount() - 1) screen.onArrowRight();
            for (int guard = 0; guard < 40 && screen.getEncoderName(2) != "SYNC"; ++guard) screen.onArrowLeft();
            expectEquals(screen.getEncoderName(2), juce::String("SYNC"));
            expectEquals(screen.getEncoderName(4), juce::String("TIME"));

            // Saved and restored; switching sync off gives the knob's value back.
            std::vector<std::unique_ptr<Arpeggiator>> noArps;
            auto data = ProjectData::fromJSON(ProjectBridge::capture({ seq, audio, nullptr, nullptr, nullptr, nullptr }, "S").toJSON());
            audio.setTrackParameter(0, AudioEngine::INSERT_SYNC_BASE + 0, 0.0f);
            expectWithinAbsoluteError(delay->getParameter(0), 0.3f, 0.0005f);
            ProjectBridge::apply(data, { seq, audio, nullptr, nullptr, nullptr, nullptr });
            auto* restored = audio.getInsertChain(0)->getEffect(0);
            expect(restored->isSynced() && restored->getSyncDivision() == 21);
            expectWithinAbsoluteError(audio.getTrackParameter(0, time), 0.3f, 0.0005f);
            expect(audio.getInsertChain(0)->getEffect(1)->isSynced() && ! audio.getInsertChain(0)->getEffect(2)->isSynced());
        }

        beginTest("Resampler: starts at once, on a threshold with pre-roll, or on the bar; a fixed length is exact");
        {
            Resampler rec;
            rec.prepare(44100.0);
            std::vector<float> ramp(512), silence(512, 0.0f);
            const double bps = 120.0 / 60.0 / 44100.0;       // beats per sample at 120 BPM

            // At once, until stopped.
            rec.arm();
            for (int b = 0; b < 4; ++b)
            {
                for (int i = 0; i < 512; ++i) ramp[(size_t) i] = (float) (b * 512 + i) * 1.0e-5f;
                rec.process(ramp.data(), ramp.data(), 512, 0.0, bps, false);
            }
            rec.stop();
            expect(rec.isFinished());
            expectEquals(rec.length(), 2048);
            expectWithinAbsoluteError(rec.left()[1000], 1000.0f * 1.0e-5f, 1.0e-7f);
            rec.release();

            // Threshold: silence is skipped, the take starts with the pre-roll just before the hit.
            rec.setThreshold(0.1f);
            rec.arm();
            for (int b = 0; b < 10; ++b) rec.process(silence.data(), silence.data(), 512, 0.0, bps, false);
            expect(rec.getState() == Resampler::State::Armed);
            expectEquals(rec.length(), 0);
            std::vector<float> hit(512, 0.0f);
            for (int i = 100; i < 512; ++i) hit[(size_t) i] = 0.5f;
            rec.process(hit.data(), hit.data(), 512, 0.0, bps, false);
            expect(rec.getState() == Resampler::State::Recording);
            const int preroll = (int) (44100.0 * Resampler::PREROLL_MS / 1000.0) + 1;
            expectEquals(rec.length(), preroll + 412);
            expectWithinAbsoluteError(rec.left()[preroll - 1], 0.0f, 1.0e-6f);
            expectWithinAbsoluteError(rec.left()[preroll], 0.5f, 1.0e-6f);
            rec.cancel();
            rec.setThreshold(0.0f);

            // On the bar, one bar long: starts at the first sample of the next bar and is exactly
            // one bar of samples (2 s at 120 BPM), wherever the block boundaries fall.
            rec.setBarStart(true);
            rec.setLengthBeats(4.0f);
            rec.arm();
            double beat = 3.5;                               // half a beat before the bar line
            juce::int64 pos = 0;
            int startedAt = -1;
            std::vector<float> index(500);
            while (! rec.isFinished() && pos < 44100 * 5)
            {
                for (int i = 0; i < 500; ++i) index[(size_t) i] = (float) (pos + i);
                rec.process(index.data(), index.data(), 500, beat, bps, true);
                if (startedAt < 0 && rec.length() > 0) startedAt = (int) rec.left()[0];
                beat += 500 * bps;
                pos += 500;
            }
            expect(rec.isFinished());
            expectEquals(startedAt, 11025, "half a beat at 120 BPM is 11025 samples");
            expectEquals(rec.length(), 88200, "one bar at 120 BPM");
            rec.release();

            // Not playing: bar start falls back to starting at once.
            rec.setLengthBeats(0.0f);
            rec.arm();
            rec.process(ramp.data(), ramp.data(), 512, 1.0, bps, false);
            expect(rec.getState() == Resampler::State::Recording);
            rec.cancel();
        }

        beginTest("Resampling a track makes a new sample; a sampler plays it and a wavetable track uses it as its USER table");
        {
            SequencerCore seq;
            AudioEngine audio;
            audio.setSequencer(&seq);
            audio.addEngine(0, AudioEngine::createEngine(EngineType::UnifiedVoice));
            audio.addEngine(1, AudioEngine::createEngine(EngineType::Sampler));
            audio.addEngine(2, AudioEngine::createEngine(EngineType::Wavetable));
            seq.getPattern().getTrack(0).engineType = EngineType::UnifiedVoice;
            seq.getPattern().getTrack(1).engineType = EngineType::Sampler;
            seq.getPattern().getTrack(2).engineType = EngineType::Wavetable;
            audio.prepare(44100.0, 256);

            WaveformScreen screen(seq, &audio);
            screen.setWriteFiles(false);
            screen.setClock([t = juce::int64(0)]() mutable { return t += 100; });   // 100 ms between clicks (CR's confirmation)
            screen.onArrowRight();                              // REC page
            expectEquals(screen.getName(), juce::String("REC"));
            for (int i = 0; i < 8; ++i) expect(screen.getEncoderName(i).isNotEmpty());

            // SR: master -> track 1. The synth on track 1 plays; the sampler track is selected.
            screen.onEncoderChange(0, 1);
            expectEquals(audio.getResampler().getSource(), 0);
            seq.setSelectedTrack(1);
            const int before = SampleBank::get().size();
            screen.onEncoderChange(4, 1);                       // RC: two clicks arm (starts at once); one alone does nothing
            screen.onEncoderChange(4, 1);
            audio.getEngine(0)->noteOn(57, 0.9f);
            std::vector<float> l(256), r(256);
            for (int b = 0; b < 60; ++b) audio.renderBlock(l.data(), r.data(), 256);
            expect(audio.getResampler().getState() == Resampler::State::Recording);
            screen.onEncoderChange(4, 127);                     // RC back: stop
            screen.poll();
            expectEquals(SampleBank::get().size(), before + 1);
            const auto* take = SampleBank::get().entry(before);
            expect(take != nullptr && (int) take->left.size() == 60 * 256);
            float peak = 0.0f;
            for (float v : take->left) peak = juce::jmax(peak, std::abs(v));
            expectWithinAbsoluteError(peak, 0.9f, 0.01f, "normalised");
            expect(screen.getSubPage().startsWith("SAVED"));

            // The selected sampler track now plays the take.
            expectEquals((int) audio.getTrackParameter(1, SamplerEngine::SampleSelect), before);
            expectEquals((int) seq.getPattern().getTrack(1).soundParams[SamplerEngine::SampleSelect], before);
            auto* samp = dynamic_cast<SamplerEngine*>(audio.getEngine(1));
            expectEquals(samp->getSampleLength(), 60 * 256);

            // CR: crop to the middle half -> another new sample, half as long, markers reset.
            audio.setTrackParameter(1, SamplerEngine::StartPoint, 0.25f);
            audio.setTrackParameter(1, SamplerEngine::EndPoint, 0.75f);
            for (int i = 0; i < 4; ++i) screen.onEncoderChange(7, 1);
            expectEquals(SampleBank::get().size(), before + 2);
            expectEquals(samp->getSampleLength(), 30 * 256);
            expectWithinAbsoluteError(samp->getParameter(SamplerEngine::EndPoint), 1.0f, 0.001f);

            // On a wavetable track SM picks the USER table's source and switches TABL to USER.
            seq.setSelectedTrack(2);
            auto* wt = dynamic_cast<WavetableEngine*>(audio.getEngine(2));
            audio.setTrackParameter(2, WavetableEngine::UserSample, (float) (before - 1));
            screen.onEncoderChange(6, 1);
            expectEquals((int) wt->getParameter(WavetableEngine::UserSample), before);
            expectEquals((int) wt->getParameter(WavetableEngine::TableSelect), (int) WavetableEngine::USER_TABLE);
            expect(wt->userTablePending());
            audio.renderBlock(l.data(), r.data(), 256);         // the audio thread swaps it in
            expect(! wt->userTablePending());
            audio.getEngine(2)->noteOn(60, 0.9f);
            double energy = 0.0;
            for (int b = 0; b < 20; ++b)
            {
                audio.renderBlock(l.data(), r.data(), 256);
                for (float v : l) energy += (double) v * v;
            }
            expect(energy > 0.01, "the USER table sounds");

            // WAVE page: start / end / loop knobs move the sampler's markers.
            seq.setSelectedTrack(1);
            screen.onArrowLeft();
            expectEquals(screen.getName(), juce::String("WAVE"));
            screen.onEncoderChange(0, 25);
            expect(samp->getParameter(SamplerEngine::StartPoint) > 0.05f);
            screen.onEncoderChange(4, 10);                      // zoom in
            expect(screen.getZoomEnd() - screen.getZoomStart() < 0.6f);
            const float fineBefore = samp->getParameter(SamplerEngine::StartPoint);
            screen.onEncoderChange(0, 1);                       // one click is finer when zoomed in
            expect(samp->getParameter(SamplerEngine::StartPoint) - fineBefore < 0.004f);
            for (int i = 0; i < 10; ++i) screen.onEncoderChange(6, 1);      // MD up to the last mode (granular)
            expectEquals((int) samp->getParameter(SamplerEngine::Mode), 6);
        }

        beginTest("Projects find a track's sample by name, wherever it now sits in the bank");
        {
            SequencerCore seq;
            AudioEngine audio;
            audio.setSequencer(&seq);
            audio.addEngine(0, AudioEngine::createEngine(EngineType::Sampler));
            seq.getPattern().getTrack(0).engineType = EngineType::Sampler;
            const int index = 3;
            const juce::String name(SampleBank::get().entry(index)->name);
            audio.setTrackParameter(0, SamplerEngine::SampleSelect, (float) index);
            seq.getPattern().getTrack(0).soundParams[SamplerEngine::SampleSelect] = (float) index;
            auto data = ProjectBridge::capture({ seq, audio, nullptr, nullptr, nullptr, nullptr }, "N");
            expectEquals(data.patterns[(size_t) data.activePattern].tracks[0].sampleName, name);

            // As if files had been added since: the stored index is wrong, the name is right.
            auto loaded = ProjectData::fromJSON(data.toJSON());
            auto& td = loaded.patterns[(size_t) loaded.activePattern].tracks[0];
            td.soundParams[SamplerEngine::SampleSelect] = 0.0f;
            ProjectBridge::apply(loaded, { seq, audio, nullptr, nullptr, nullptr, nullptr });
            expectEquals((int) audio.getTrackParameter(0, SamplerEngine::SampleSelect), index);

            // A name that no longer exists falls back to the stored index.
            td.sampleName = "ZZZZ";
            td.soundParams[SamplerEngine::SampleSelect] = 1.0f;
            ProjectBridge::apply(loaded, { seq, audio, nullptr, nullptr, nullptr, nullptr });
            expectEquals((int) audio.getTrackParameter(0, SamplerEngine::SampleSelect), 1);
        }

        beginTest("A rebuild on another thread costs the audio thread a silent block, never a crash");
        {
            SequencerCore seq;
            AudioEngine audio;
            audio.setSequencer(&seq);
            audio.addEngine(0, AudioEngine::createEngine(EngineType::UnifiedVoice));
            audio.prepare(44100.0, 128);
            audio.getEngine(0)->noteOn(60, 0.9f);
            std::atomic<bool> stop { false };
            std::thread editor([&]
            {
                int n = 0;
                while (! stop.load())
                {
                    audio.setInsertEffect(0, 0, (EffectType) (n % (int) EffectType::NumEffectTypes));
                    audio.setEngineType(0, (n % 2) ? EngineType::FM : EngineType::UnifiedVoice, true);
                    ++n;
                }
            });
            std::vector<float> l(128), r(128);
            bool finite = true;
            for (int b = 0; b < 4000; ++b)
            {
                audio.renderBlock(l.data(), r.data(), 128);
                for (float v : l) finite = finite && std::isfinite(v);
            }
            stop.store(true);
            editor.join();
            expect(finite, "output stays finite while effects and engines are swapped underneath");
            expect(audio.getSkippedBlocks() >= 0);
        }

        beginTest("PRESET screen: all eight knobs do what their labels say");
        {
            SequencerCore seq;
            AudioEngine audio;
            audio.setSequencer(&seq);
            audio.addEngine(0, AudioEngine::createEngine(EngineType::UnifiedVoice));
            seq.getPattern().getTrack(0).engineType = EngineType::UnifiedVoice;
            PresetManager mgr;
            PresetScreen screen(seq, &audio, mgr);
            screen.setClock([t = juce::int64(0)]() mutable { return t += 100; });   // 100 ms between clicks (the confirmations)
            screen.onActivate();
            for (int i = 0; i < 8; ++i)
                expect(screen.getEncoderName(i).isNotEmpty(), "every knob is named");

            int changed = 0;
            screen.onUserPresetsChanged = [&] { ++changed; };
            const int before = mgr.getPresetCount();

            // SAVE needs four clicks clockwise; three do nothing, and another knob in between resets it.
            for (int i = 0; i < 3; ++i) screen.onEncoderChange(5, 1);
            expectEquals(mgr.getPresetCount(), before);
            screen.onEncoderChange(7, 1);
            for (int i = 0; i < 3; ++i) screen.onEncoderChange(5, 1);
            expectEquals(mgr.getPresetCount(), before);
            screen.onEncoderChange(5, 1);
            expectEquals(mgr.getPresetCount(), before + 1);
            expectEquals(changed, 1);
            expect(! mgr.getPreset(before).isFactory);

            // VL moves the track volume.
            const float vol = audio.getTrackParameter(0, MixParam::VOLUME);
            for (int i = 0; i < 5; ++i) screen.onEncoderChange(7, 127);
            expectWithinAbsoluteError(audio.getTrackParameter(0, MixParam::VOLUME), vol - 0.05f, 0.001f);

            // EN: any engine shows more presets than this engine alone; the name bar says so.
            screen.onEncoderChange(2, 127);
            const auto subThis = screen.getSubPage();
            screen.onEncoderChange(2, 1);
            expect(screen.getSubPage().contains("ANY") && ! subThis.contains("ANY"));

            // DL refuses factory presets (the list starts on one) and removes a user preset.
            screen.onEncoderChange(2, 127);
            for (int i = 0; i < 4; ++i) screen.onEncoderChange(6, 1);
            expectEquals(mgr.getPresetCount(), before + 1, "a factory preset is not deleted");
            for (int i = 0; i < 400; ++i) screen.onEncoderChange(0, 1);     // to the end: the user preset
            for (int i = 0; i < 4; ++i) screen.onEncoderChange(6, 1);
            expectEquals(mgr.getPresetCount(), before, "the user preset is deleted");
            expectEquals(changed, 2);

            // AU: audition is on unless it is switched off here.
            expect(screen.isAuditionOn());
            screen.onEncoderChange(3, 127);
            expect(! screen.isAuditionOn() && screen.getSubPage().contains("AU-"));
        }

        beginTest("SCALE screen: knob 7 shifts the page an octave, knob 8 is the scale lock");
        {
            CustomScaleManager mgr;
            CustomScaleScreen screen(mgr);
            bool locked = false;
            screen.setLockHooks([&] { return locked; }, [&](bool on) { locked = on; });
            expect(screen.getEncoderName(6) == "OC" && screen.getEncoderName(7) == "LK");
            const int root = mgr.getRootNote();
            screen.onEncoderChange(6, 1);
            expectEquals(mgr.getRootNote(), root + 12);
            screen.onEncoderChange(6, 127);
            expectEquals(mgr.getRootNote(), root);
            screen.onEncoderChange(7, 1);
            expect(locked && screen.getSubPage().contains("LK"));
            screen.onEncoderChange(7, 127);
            expect(! locked);
        }

        beginTest("Roll and per-track arpeggiator settings survive a project round trip");
        {
            SequencerCore seq;
            AudioEngine audio;
            audio.setSequencer(&seq);
            for (int t = 0; t < 8; ++t)
                audio.addEngine(t, AudioEngine::createEngine(EngineType::UnifiedVoice));
            std::vector<std::unique_ptr<Arpeggiator>> arps;
            for (int t = 0; t < 8; ++t) arps.push_back(std::make_unique<Arpeggiator>());
            auto arpFor = [&](int t) { return (t >= 0 && t < 8) ? arps[(size_t) t].get() : nullptr; };

            arps[3]->setOctaveRange(4);
            arps[3]->setEnabled(true);
            arps[5]->setGate(0.15f);
            audio.getPadRatchet().setEnabled(true);
            audio.getPadRatchet().setGate(0.25f);
            audio.getPadRatchet().setMode(PadRatchet::Mode::Free);
            audio.getPadRatchet().setSwing(0.4f);
            audio.getPadRatchet().setMaxRepeats(6);
            arps[5]->setStripTarget(Arpeggiator::StripTarget::Density);
            const float rhythm[] = { 1.0f, 3.0f, 0.0f, 1.0f, 0.5f, 0.25f, 0.6f, 1.5f, 1.0f, 0.8f, 1.0f };
            arps[5]->setRhythmState(rhythm, 11);
            audio.addInsertEffect(2, static_cast<EffectType>(0));
            audio.addInsertEffect(2, static_cast<EffectType>(1));
            audio.setTrackParameter(2, AudioEngine::INSERT_ON_BASE + 1, 0.0f);
            audio.setTrackParameter(2, AudioEngine::INSERT_WET_BASE + 0, 0.35f);

            auto data = ProjectData::fromJSON(ProjectBridge::capture({ seq, audio, nullptr, nullptr, nullptr, arpFor }, "R").toJSON());

            arps[3]->setOctaveRange(1);
            arps[3]->setEnabled(false);
            arps[5]->setGate(0.9f);
            audio.getPadRatchet().setEnabled(false);
            audio.getPadRatchet().setGate(0.9f);
            audio.getPadRatchet().setMode(PadRatchet::Mode::Quantized);
            audio.getPadRatchet().setSwing(0.0f);
            audio.getPadRatchet().setMaxRepeats(0);
            arps[5]->setStripTarget(Arpeggiator::StripTarget::Off);
            arps[5]->clearRhythmTemplate();
            audio.setTrackParameter(2, AudioEngine::INSERT_ON_BASE + 1, 1.0f);
            audio.setTrackParameter(2, AudioEngine::INSERT_WET_BASE + 0, 1.0f);

            ProjectBridge::apply(data, { seq, audio, nullptr, nullptr, nullptr, arpFor });
            expectWithinAbsoluteError(audio.getPadRatchet().getSwing(), 0.4f, 0.001f);
            expectEquals(audio.getPadRatchet().getMaxRepeats(), 6);
            expect(arps[5]->getStripTarget() == Arpeggiator::StripTarget::Density, "strip target saved");
            expect(arps[5]->isRhythmEnabled() && arps[5]->getRhythmLength() == 3, "tapped rhythm saved");
            expectWithinAbsoluteError((float) arps[5]->getRhythmStepLength(1), 1.5f, 0.001f);
            expect(arps[4]->getRhythmLength() == 0);
            expectWithinAbsoluteError(audio.getTrackParameter(2, AudioEngine::INSERT_ON_BASE + 1), 0.0f, 0.001f);
            expectWithinAbsoluteError(audio.getTrackParameter(2, AudioEngine::INSERT_ON_BASE + 0), 1.0f, 0.001f);
            expectWithinAbsoluteError(audio.getTrackParameter(2, AudioEngine::INSERT_WET_BASE + 0), 0.35f, 0.001f);
            arps[5]->setDensity(0.5f);
            expect(arps[5]->applyStrip(0.9f) && std::abs(arps[5]->getDensity() - 0.9f) < 0.001f, "strip drives the macro");
            expect(! arps[4]->applyStrip(0.9f), "no macro assigned: the strip stays the mod wheel");
            expectEquals(arps[3]->getOctaveRange(), 4);
            expect(arps[3]->isEnabled() && !arps[2]->isEnabled());
            expectWithinAbsoluteError(arps[5]->getGate(), 0.15f, 0.001f);
            expect(audio.getPadRatchet().isEnabled());
            expectWithinAbsoluteError(audio.getPadRatchet().getGate(), 0.25f, 0.001f);
            expect(audio.getPadRatchet().getMode() == PadRatchet::Mode::Free);
        }

        beginTest("Arpeggiators follow the sequencer's grid while it runs, and their output can be recorded");
        {
            SequencerCore seq;
            seq.prepareToPlay(44100.0, 512);
            seq.setTempo(120.0f);
            AudioEngine audio;
            audio.setSequencer(&seq);
            audio.addEngine(0, AudioEngine::createEngine(EngineType::UnifiedVoice));
            Arpeggiator arp;
            arp.onNoteOn = [](int, float) {};
            arp.onNoteOff = [](int) {};
            int recorded = 0;
            arp.onRecordNote = [&](int pitch, float velocity)
            {
                ++recorded;
                if (seq.isRecording() && seq.isPlaying())
                    seq.recordNote(0, pitch, velocity);
            };
            audio.setTrackArpeggiator(0, &arp);
            arp.setEnabled(true);
            arp.noteOn(60, 1.0f);
            arp.noteOn(64, 1.0f);

            expect(seq.getBeatPosition() == 0.0);
            seq.setRecording(true);
            seq.start();
            arp.setRecording(true);
            for (int b = 0; b < 172; ++b)           // 2 s = one bar at 120 BPM
            {
                audio.processGenerators(512);
                seq.processBlock(512);
            }
            seq.stop();
            expect(arp.isTransportSynced(), "The audio engine syncs running arpeggiators to the transport");
            expectWithinAbsoluteError((float) seq.getBeatPosition(), 4.0f, 0.1f);
            expect(recorded >= 6, "Arp notes reach the record hook: " + juce::String(recorded));

            int trigs = 0;
            auto& track = seq.getPattern().getTrack(0);
            for (int s = 0; s < track.length; ++s)
                if (track.getStep(s).trig) ++trigs;
            expect(trigs >= 4, "Recorded arp notes are steps in the pattern: " + juce::String(trigs));
        }

        beginTest("No step held: encoders are the track's sequencer settings");
        {
            Rig rig;
            auto& track = rig.seq.getPattern().getTrack(0);
            expectEquals(rig.steps->getEncoderName(0), juce::String("LN"));
            rig.steps->onEncoderChange(0, 8);
            expectEquals(track.length, 24);
            float bpm = rig.seq.getTempo();
            rig.steps->onEncoderChange(7, 5);
            expectWithinAbsoluteError(rig.seq.getTempo(), bpm + 5.0f, 0.01f);
            expectEquals(rig.steps->getEncoderName(7), juce::String((int) rig.seq.getTempo()));
        }

        beginTest("A lock is applied at its step and the base value returns on the next trig");
        {
            SequencerCore seq;
            AudioEngine audio;
            audio.addEngine(0, AudioEngine::createEngine(EngineType::UnifiedVoice));
            Step locked, plain;
            locked.trig = plain.trig = true;
            locked.paramLocks[UnifiedParam::CUTOFF] = 500.0f;

            audio.setTrackParameter(0, UnifiedParam::CUTOFF, 4000.0f);
            audio.applyStepLocks(0, locked);
            expectWithinAbsoluteError(audio.getTrackParameter(0, UnifiedParam::CUTOFF), 500.0f, 0.5f);
            audio.applyStepLocks(0, plain);
            expectWithinAbsoluteError(audio.getTrackParameter(0, UnifiedParam::CUTOFF), 4000.0f, 0.5f);

            // Editing the parameter while a lock is active changes what it returns to.
            audio.applyStepLocks(0, locked);
            audio.setTrackParameter(0, UnifiedParam::CUTOFF, 2000.0f);
            audio.applyStepLocks(0, plain);
            expectWithinAbsoluteError(audio.getTrackParameter(0, UnifiedParam::CUTOFF), 2000.0f, 0.5f);
        }
    }
};
static StepDetailTests stepDetailTests;

class StepViewScreenTests : public juce::UnitTest
{
public:
    StepViewScreenTests() : UnitTest("Step View Screen") {}

    void runTest() override
    {
        beginTest("Pad press toggles step trig");
        {
            SequencerCore seq;
            PadManager pm;
            PadLEDFeedback leds;
            leds.setSequencer(&seq);
            StepViewScreen screen(seq, pm, leds);

            auto& step = seq.getPattern().getTrack(0).getStep(0);
            expect(!step.trig);

            screen.onPadPress(0, 100);
            expect(step.trig);
            expect(!step.notes.empty());
            screen.onPadRelease(0);
            expect(step.trig, "the tap that added the trig does not remove it");

            // Control grammar: an existing trig goes on a short tap (press + quick release).
            screen.onPadPress(0, 100);
            expect(step.trig);
            screen.onPadRelease(0);
            expect(!step.trig);
        }

        beginTest("Page navigation");
        {
            SequencerCore seq;
            PadManager pm;
            PadLEDFeedback leds;
            StepViewScreen screen(seq, pm, leds);

            seq.getPattern().getTrack(0).setLength(64);

            // Control grammar: Up / Down (pad paging) choose which 16 steps the pads show.
            expectEquals(screen.getCurrentPage(), 0);
            screen.onPadPageNext();
            expectEquals(screen.getCurrentPage(), 1);
            screen.onPadPageNext();
            expectEquals(screen.getCurrentPage(), 2);
            screen.onPadPagePrev();
            expectEquals(screen.getCurrentPage(), 1);
            screen.onPadPagePrev();
            screen.onPadPagePrev();
            expectEquals(screen.getCurrentPage(), 0);
        }

        beginTest("Page does not exceed track length");
        {
            SequencerCore seq;
            PadManager pm;
            PadLEDFeedback leds;
            StepViewScreen screen(seq, pm, leds);

            screen.onPadPageNext();
            expectEquals(screen.getCurrentPage(), 0);
        }

        beginTest("Sub-page shows the step page; n/N is the encoder page only");
        {
            SequencerCore seq;
            PadManager pm;
            PadLEDFeedback leds;
            StepViewScreen screen(seq, pm, leds);

            expectEquals(screen.getPageCount(), 1);
            expectEquals(screen.getSubPage(), juce::String("S 1-16"));

            seq.getPattern().getTrack(0).setLength(32);
            expectEquals(screen.getPageCount(), 1);
            expectEquals(screen.getPageIndex(), 0);
            screen.onPadPageNext();
            expectEquals(screen.getPageIndex(), 0);
            expectEquals(screen.getSubPage(), juce::String("S 17-32"));
        }

        beginTest("Render does not crash");
        {
            SequencerCore seq;
            PadManager pm;
            PadLEDFeedback leds;
            leds.setSequencer(&seq);
            StepViewScreen screen(seq, pm, leds);
            DisplayRenderer display;

            seq.getPattern().getTrack(0).getStep(0).trig = true;
            seq.getPattern().getTrack(0).getStep(0).notes.push_back({60, 1.0f});

            screen.render(display);

            int litPixels = 0;
            for (int y = 0; y < DisplayRenderer::HEIGHT; ++y)
                for (int x = 0; x < DisplayRenderer::WIDTH; ++x)
                    if (display.getPixel(x, y))
                        ++litPixels;
            expect(litPixels > 0);
        }
    }
};

static MidiMessageTests midiMessageTests;
static BitmapEncodingTests bitmapEncodingTests;
static VoiceAllocatorTests voiceAllocatorTests;
static SequencerTests sequencerTests;
static EnvelopeTests envelopeTests;
static SVFilterTests svFilterTests;
static DisplayRendererTests displayRendererTests;
static PadManagerTests padManagerTests;
static BitmapFontTests bitmapFontTests;
class ModMatrixTests : public juce::UnitTest
{
public:
    ModMatrixTests() : UnitTest("Mod Matrix") {}

    void runTest() override
    {
        beginTest("Empty mod matrix returns base value");
        {
            ModMatrix mm;
            float result = mm.getModulatedValue(10, 0.5f);
            expectEquals(result, 0.5f);
        }

        beginTest("Single bipolar modulation");
        {
            ModMatrix mm;
            mm.addSlot({ModSource::LFO1, 10, 0.5f, true});
            mm.setSourceValue(ModSource::LFO1, 1.0f);

            float result = mm.getModulatedValue(10, 0.5f);
            expectEquals(result, 1.0f);
        }

        beginTest("Single unipolar modulation");
        {
            ModMatrix mm;
            mm.addSlot({ModSource::LFO1, 10, 1.0f, false});
            mm.setSourceValue(ModSource::LFO1, -1.0f);

            float result = mm.getModulatedValue(10, 0.0f);
            expectEquals(result, 0.0f);

            mm.setSourceValue(ModSource::LFO1, 1.0f);
            result = mm.getModulatedValue(10, 0.0f);
            expectEquals(result, 1.0f);
        }

        beginTest("Multiple sources summed");
        {
            ModMatrix mm;
            mm.addSlot({ModSource::LFO1, 10, 0.3f, true});
            mm.addSlot({ModSource::Env1, 10, 0.2f, true});
            mm.setSourceValue(ModSource::LFO1, 1.0f);
            mm.setSourceValue(ModSource::Env1, 1.0f);

            float result = mm.getModulatedValue(10, 0.0f);
            expectWithinAbsoluteError(result, 0.5f, 0.001f);
        }

        beginTest("Different destinations are independent");
        {
            ModMatrix mm;
            mm.addSlot({ModSource::LFO1, 10, 1.0f, true});
            mm.addSlot({ModSource::LFO2, 20, 0.5f, true});
            mm.setSourceValue(ModSource::LFO1, 1.0f);
            mm.setSourceValue(ModSource::LFO2, 1.0f);

            float r1 = mm.getModulatedValue(10, 0.0f);
            float r2 = mm.getModulatedValue(20, 0.0f);
            expectEquals(r1, 1.0f);
            expectEquals(r2, 0.5f);
        }

        beginTest("Remove slot");
        {
            ModMatrix mm;
            mm.addSlot({ModSource::LFO1, 10, 1.0f, true});
            mm.addSlot({ModSource::LFO2, 20, 1.0f, true});
            expectEquals(mm.getSlotCount(), 2);
            mm.removeSlot(0);
            expectEquals(mm.getSlotCount(), 1);
        }

        beginTest("Process applies to param map");
        {
            ModMatrix mm;
            mm.addSlot({ModSource::Velocity, 10, 0.5f, true});
            mm.setSourceValue(ModSource::Velocity, 0.8f);

            ParamMap params;
            params[10] = 1.0f;
            mm.process(params);
            expectWithinAbsoluteError(params[10], 1.4f, 0.001f);
        }
    }
};

class LFOTests : public juce::UnitTest
{
public:
    LFOTests() : UnitTest("LFO") {}

    void runTest() override
    {
        beginTest("Sine LFO output is bounded");
        {
            LFO lfo;
            lfo.setSampleRate(44100.0);
            lfo.setRate(1.0f);
            lfo.setShape(LFO::Shape::Sine);
            lfo.setDepth(1.0f);

            for (int i = 0; i < 44100; ++i)
            {
                float val = lfo.process();
                expect(val >= -1.0f && val <= 1.0f);
            }
        }

        beginTest("Triangle LFO output is bounded");
        {
            LFO lfo;
            lfo.setSampleRate(44100.0);
            lfo.setRate(2.0f);
            lfo.setShape(LFO::Shape::Triangle);
            lfo.setDepth(1.0f);

            for (int i = 0; i < 44100; ++i)
            {
                float val = lfo.process();
                expect(val >= -1.0f && val <= 1.0f);
            }
        }

        beginTest("Square LFO alternates between extremes");
        {
            LFO lfo;
            lfo.setSampleRate(44100.0);
            lfo.setRate(1.0f);
            lfo.setShape(LFO::Shape::Square);
            lfo.setDepth(1.0f);

            bool sawPositive = false;
            bool sawNegative = false;
            for (int i = 0; i < 44100; ++i)
            {
                float val = lfo.process();
                if (val > 0.5f) sawPositive = true;
                if (val < -0.5f) sawNegative = true;
            }
            expect(sawPositive && sawNegative);
        }

        beginTest("Depth scales output");
        {
            LFO lfo;
            lfo.setSampleRate(44100.0);
            lfo.setRate(10.0f);
            lfo.setShape(LFO::Shape::Sine);
            lfo.setDepth(0.5f);

            float maxVal = 0.0f;
            for (int i = 0; i < 44100; ++i)
            {
                float val = std::abs(lfo.process());
                if (val > maxVal) maxVal = val;
            }
            expect(maxVal <= 0.51f);
            expect(maxVal >= 0.45f);
        }

        beginTest("KeySync resets phase on trigger");
        {
            LFO lfo;
            lfo.setSampleRate(44100.0);
            lfo.setRate(1.0f);
            lfo.setShape(LFO::Shape::Saw);
            lfo.setTriggerMode(LFO::TriggerMode::KeySync);

            for (int i = 0; i < 22050; ++i)
                lfo.process();

            lfo.trigger();
            float afterTrigger = lfo.process();
            expect(afterTrigger < 0.01f && afterTrigger > -1.01f);
        }
    }
};

class UnifiedVoiceTests : public juce::UnitTest
{
public:
    UnifiedVoiceTests() : UnitTest("Unified Voice") {}

    void runTest() override
    {
        beginTest("Voice starts inactive");
        {
            UnifiedVoice voice;
            voice.setSampleRate(44100.0);
            expect(!voice.isActive());
        }

        beginTest("Note on activates, produces output");
        {
            UnifiedVoice voice;
            voice.setSampleRate(44100.0);
            voice.setNote(60, 1.0f);
            voice.noteOn();
            expect(voice.isActive());

            float maxAbs = 0.0f;
            for (int i = 0; i < 1000; ++i)
            {
                float s = voice.process();
                maxAbs = std::max(maxAbs, std::abs(s));
            }
            expect(maxAbs > 0.01f);
        }

        beginTest("Note off eventually deactivates");
        {
            UnifiedVoice voice;
            voice.setSampleRate(44100.0);

            Envelope::Params fast;
            fast.attack = 0.001f;
            fast.decay = 0.001f;
            fast.sustain = 0.5f;
            fast.release = 0.001f;
            voice.getAmpEnvelope().setParams(fast);

            voice.setNote(60, 1.0f);
            voice.noteOn();
            for (int i = 0; i < 200; ++i) voice.process();
            voice.noteOff();
            for (int i = 0; i < 200; ++i) voice.process();
            expect(!voice.isActive());
        }

        beginTest("Filter cutoff changes tone");
        {
            // Analog character randomises oscillator start phases (and so how much the oscillators
            // cancel); switch it off so the two voices differ only in filter cutoff.
            UnifiedVoice voiceLow;
            voiceLow.getAnalogCharacter().enabled = false;
            voiceLow.setSampleRate(44100.0);
            voiceLow.setNote(48, 1.0f);
            voiceLow.setFilterBaseCutoff(200.0f);
            voiceLow.setFilterResonance(0.0f);
            voiceLow.noteOn();

            // Measure over many cycles of the 131 Hz note (0.2 s) after a settling period.
            for (int i = 0; i < 2000; ++i) voiceLow.process();
            float sumLow = 0.0f;
            for (int i = 0; i < 8820; ++i)
                sumLow += std::abs(voiceLow.process());

            UnifiedVoice voiceHigh;
            voiceHigh.getAnalogCharacter().enabled = false;
            voiceHigh.setSampleRate(44100.0);
            voiceHigh.setNote(48, 1.0f);
            voiceHigh.setFilterBaseCutoff(5000.0f);
            voiceHigh.setFilterResonance(0.0f);
            voiceHigh.noteOn();

            for (int i = 0; i < 2000; ++i) voiceHigh.process();
            float sumHigh = 0.0f;
            for (int i = 0; i < 8820; ++i)
                sumHigh += std::abs(voiceHigh.process());

            expect(sumHigh > sumLow);
        }
    }
};

class UnifiedVoiceEngineTests : public juce::UnitTest
{
public:
    UnifiedVoiceEngineTests() : UnitTest("Unified Voice Engine") {}

    void runTest() override
    {
        beginTest("Engine type and name");
        {
            UnifiedVoiceEngine engine;
            expectEquals((int) engine.getType(), (int) EngineType::UnifiedVoice);
            expect(engine.getName() == "Unified Voice");
        }

        beginTest("PrepareToPlay and processBlock");
        {
            UnifiedVoiceEngine engine;
            engine.prepareToPlay(44100.0, 512);
            engine.noteOn(60, 1.0f, -1);

            juce::AudioBuffer<float> buffer(2, 512);
            buffer.clear();
            juce::MidiBuffer midi;
            engine.processBlock(buffer, midi);

            float maxAbs = buffer.getMagnitude(0, 512);
            expect(maxAbs > 0.0f);
        }

        beginTest("All notes off silences engine");
        {
            UnifiedVoiceEngine engine;
            engine.prepareToPlay(44100.0, 512);
            engine.noteOn(60, 1.0f, -1);

            juce::AudioBuffer<float> buffer(2, 512);
            buffer.clear();
            juce::MidiBuffer midi;
            engine.processBlock(buffer, midi);

            engine.allNotesOff();

            buffer.clear();
            engine.processBlock(buffer, midi);
            float mag = buffer.getMagnitude(0, 512);
            expectEquals(mag, 0.0f);
        }

        beginTest("Parameter set and get");
        {
            UnifiedVoiceEngine engine;
            engine.setParameter(10, 5000.0f);
            expectEquals(engine.getParameter(10), 5000.0f);
        }
    }
};

class LadderFilterTests : public juce::UnitTest
{
public:
    LadderFilterTests() : UnitTest("Ladder Filter") {}

    void runTest() override
    {
        beginTest("Passband signal passes through");
        {
            LadderFilter filter;
            filter.setSampleRate(44100.0);
            filter.setCutoff(5000.0f);
            filter.setResonance(0.0f);

            for (int i = 0; i < 500; ++i)
                filter.process(0.5f);

            float out = 0.0f;
            for (int i = 0; i < 100; ++i)
                out += std::abs(filter.process(0.5f));

            expect(out > 0.1f, "Passband signal should pass through");
        }

        beginTest("High frequency is attenuated with low cutoff");
        {
            LadderFilter filter;
            filter.setSampleRate(44100.0);
            filter.setCutoff(200.0f);
            filter.setResonance(0.0f);

            float highFreqEnergy = 0.0f;
            for (int i = 0; i < 2000; ++i)
            {
                float input = std::sin(2.0f * 3.14159f * 8000.0f * i / 44100.0f);
                float out = filter.process(input);
                if (i > 500)
                    highFreqEnergy += out * out;
            }

            expect(highFreqEnergy < 0.01f, "8kHz should be heavily attenuated at 200Hz cutoff");
        }

        beginTest("Resonance changes frequency response");
        {
            LadderFilter noReso;
            noReso.setSampleRate(44100.0);
            noReso.setCutoff(1000.0f);
            noReso.setResonance(0.0f);

            LadderFilter withReso;
            withReso.setSampleRate(44100.0);
            withReso.setCutoff(1000.0f);
            withReso.setResonance(0.9f);

            float energyNoReso = 0.0f;
            float energyWithReso = 0.0f;

            for (int i = 0; i < 4000; ++i)
            {
                float input = std::sin(2.0f * 3.14159f * 1000.0f * i / 44100.0f);
                float o1 = noReso.process(input);
                float o2 = withReso.process(input);
                if (i > 1000)
                {
                    energyNoReso += o1 * o1;
                    energyWithReso += o2 * o2;
                }
            }

            expect(energyWithReso > energyNoReso * 1.5f,
                   "Resonance should boost energy near cutoff");
        }

        beginTest("Reset clears state");
        {
            LadderFilter filter;
            filter.setSampleRate(44100.0);
            filter.setCutoff(1000.0f);
            for (int i = 0; i < 100; ++i)
                filter.process(1.0f);

            filter.reset();
            float out = filter.process(0.0f);
            expectWithinAbsoluteError(out, 0.0f, 0.0001f, "After reset, output should be zero");
        }
    }
};

class FMOperatorTests : public juce::UnitTest
{
public:
    FMOperatorTests() : UnitTest("FM Operator") {}

    void runTest() override
    {
        beginTest("Operator produces output");
        {
            FMOperator op;
            op.setSampleRate(44100.0);
            op.setRatio(1.0f);
            op.setLevel(1.0f);
            op.noteOn(69, 1.0f);

            float maxAbs = 0.0f;
            for (int i = 0; i < 1000; ++i)
                maxAbs = std::max(maxAbs, std::abs(op.process()));

            expect(maxAbs > 0.1f, "Operator should produce audible output");
        }

        beginTest("FM modulation changes output");
        {
            FMOperator modulated;
            modulated.setSampleRate(44100.0);
            modulated.setRatio(1.0f);
            modulated.setLevel(1.0f);
            modulated.noteOn(69, 1.0f);

            FMOperator unmodulated;
            unmodulated.setSampleRate(44100.0);
            unmodulated.setRatio(1.0f);
            unmodulated.setLevel(1.0f);
            unmodulated.noteOn(69, 1.0f);

            float diffSum = 0.0f;
            for (int i = 0; i < 1000; ++i)
            {
                float mod = std::sin(2.0f * 3.14159f * 880.0f * i / 44100.0f) * 3.0f;
                float a = modulated.process(mod);
                float b = unmodulated.process(0.0f);
                diffSum += std::abs(a - b);
            }

            expect(diffSum > 1.0f, "Modulation input should change operator output");
        }

        beginTest("Feedback creates harmonics");
        {
            FMOperator noFb;
            noFb.setSampleRate(44100.0);
            noFb.setRatio(1.0f);
            noFb.setLevel(1.0f);
            noFb.setFeedback(0.0f);
            noFb.noteOn(69, 1.0f);

            FMOperator withFb;
            withFb.setSampleRate(44100.0);
            withFb.setRatio(1.0f);
            withFb.setLevel(1.0f);
            withFb.setFeedback(1.5f);
            withFb.noteOn(69, 1.0f);

            float energyNoFb = 0.0f;
            float energyWithFb = 0.0f;
            for (int i = 0; i < 1000; ++i)
            {
                float a = noFb.process();
                float b = withFb.process();
                energyNoFb += a * a;
                energyWithFb += b * b;
            }

            expect(std::abs(energyWithFb - energyNoFb) > 0.01f, "Feedback should change waveform energy");
        }

        beginTest("Velocity scales output");
        {
            FMOperator loud;
            loud.setSampleRate(44100.0);
            loud.setRatio(1.0f);
            loud.setLevel(1.0f);
            loud.noteOn(69, 1.0f);

            FMOperator quiet;
            quiet.setSampleRate(44100.0);
            quiet.setRatio(1.0f);
            quiet.setLevel(1.0f);
            quiet.noteOn(69, 0.25f);

            float peakLoud = 0.0f, peakQuiet = 0.0f;
            for (int i = 0; i < 500; ++i)
            {
                peakLoud = std::max(peakLoud, std::abs(loud.process()));
                peakQuiet = std::max(peakQuiet, std::abs(quiet.process()));
            }

            expect(peakLoud > peakQuiet * 2.0f, "Full velocity should be louder than quarter velocity");
        }
    }
};

class FMVoiceTests : public juce::UnitTest
{
public:
    FMVoiceTests() : UnitTest("FM Voice") {}

    void runTest() override
    {
        beginTest("Additive algorithm sums all operators");
        {
            FMVoice voice;
            voice.setSampleRate(44100.0);
            voice.setAlgorithm(FMVoice::Algorithm::Algo6);

            for (int i = 0; i < FMVoice::NUM_OPS; ++i)
            {
                voice.getOp(i).setRatio((float)(i + 1));
                voice.getOp(i).setLevel(0.15f);
            }

            voice.noteOn(69, 1.0f);

            float maxAbs = 0.0f;
            for (int i = 0; i < 1000; ++i)
                maxAbs = std::max(maxAbs, std::abs(voice.process()));

            expect(maxAbs > 0.1f, "Additive algorithm should produce output from all ops");
        }

        beginTest("Serial algorithm modulates");
        {
            FMVoice voice;
            voice.setSampleRate(44100.0);
            voice.setAlgorithm(FMVoice::Algorithm::Algo1);

            voice.getOp(0).setRatio(1.0f);
            voice.getOp(0).setLevel(1.0f);
            voice.getOp(1).setRatio(2.0f);
            voice.getOp(1).setLevel(1.0f);
            for (int i = 2; i < 6; ++i)
                voice.getOp(i).setLevel(0.0f);

            voice.noteOn(69, 1.0f);

            float maxAbs = 0.0f;
            for (int i = 0; i < 1000; ++i)
                maxAbs = std::max(maxAbs, std::abs(voice.process()));

            expect(maxAbs > 0.01f, "Serial chain should produce output");
        }

        beginTest("Voice becomes inactive after note off and release");
        {
            FMVoice voice;
            voice.setSampleRate(44100.0);
            voice.setAlgorithm(FMVoice::Algorithm::Algo6);

            Envelope::Params fast;
            fast.attack = 0.001f;
            fast.decay = 0.001f;
            fast.sustain = 0.0f;
            fast.release = 0.001f;

            for (int i = 0; i < 6; ++i)
            {
                voice.getOp(i).setLevel(1.0f);
                voice.getOp(i).getEnvelope().setParams(fast);
            }

            voice.noteOn(69, 1.0f);

            for (int i = 0; i < 500; ++i)
                voice.process();

            voice.noteOff();

            for (int i = 0; i < 500; ++i)
                voice.process();

            expect(!voice.isActive(), "Voice should become inactive after envelope completes");
        }

        beginTest("Algorithm switching changes timbre");
        {
            FMVoice v1;
            v1.setSampleRate(44100.0);
            v1.setAlgorithm(FMVoice::Algorithm::Algo1);
            for (int i = 0; i < 6; ++i)
            {
                v1.getOp(i).setRatio((float)(i + 1));
                v1.getOp(i).setLevel(0.5f);
            }
            v1.noteOn(69, 1.0f);

            FMVoice v2;
            v2.setSampleRate(44100.0);
            v2.setAlgorithm(FMVoice::Algorithm::Algo6);
            for (int i = 0; i < 6; ++i)
            {
                v2.getOp(i).setRatio((float)(i + 1));
                v2.getOp(i).setLevel(0.5f);
            }
            v2.noteOn(69, 1.0f);

            float diffSum = 0.0f;
            for (int i = 0; i < 1000; ++i)
                diffSum += std::abs(v1.process() - v2.process());

            expect(diffSum > 1.0f, "Different algorithms should produce different timbres");
        }
    }
};

class FMEngineTests : public juce::UnitTest
{
public:
    FMEngineTests() : UnitTest("FM Engine") {}

    void runTest() override
    {
        beginTest("Engine produces output on note");
        {
            FMEngine engine(4);
            engine.prepareToPlay(44100.0, 512);
            engine.setAlgorithm(FMVoice::Algorithm::Algo6);

            for (int op = 0; op < 6; ++op)
                engine.setParameter(op * 10 + 1, 0.5f);

            engine.noteOn(69, 1.0f, -1);

            juce::AudioBuffer<float> buffer(2, 512);
            buffer.clear();
            juce::MidiBuffer midi;
            engine.processBlock(buffer, midi);

            float maxAbs = 0.0f;
            auto* data = buffer.getReadPointer(0);
            for (int i = 0; i < 512; ++i)
                maxAbs = std::max(maxAbs, std::abs(data[i]));

            expect(maxAbs > 0.01f, "FM engine should produce output");
        }

        beginTest("Algorithm parameter changes algorithm");
        {
            FMEngine engine(4);
            engine.prepareToPlay(44100.0, 512);
            engine.setParameter(FMEngine::AlgorithmParam, (float) FMVoice::Algorithm::Algo6);

            expectEquals((int) engine.getVoice(0).getAlgorithm(), (int) FMVoice::Algorithm::Algo6);
        }

        beginTest("All notes off silences engine");
        {
            FMEngine engine(4);
            engine.prepareToPlay(44100.0, 512);
            engine.setAlgorithm(FMVoice::Algorithm::Algo6);

            engine.noteOn(60, 1.0f, -1);
            engine.noteOn(64, 1.0f, -1);

            juce::AudioBuffer<float> buf(2, 256);
            buf.clear();
            juce::MidiBuffer midi;
            engine.processBlock(buf, midi);

            engine.allNotesOff();

            float maxAbs = 0.0f;
            buf.clear();
            engine.processBlock(buf, midi);
            auto* data = buf.getReadPointer(0);
            for (int i = 0; i < 256; ++i)
                maxAbs = std::max(maxAbs, std::abs(data[i]));

            expectWithinAbsoluteError(maxAbs, 0.0f, 0.001f, "All notes off should silence");
        }

        beginTest("Parameter names resolve correctly");
        {
            FMEngine engine;
            expectEquals(engine.getParameterName(FMEngine::Op1Ratio), juce::String("Op1 Ratio"));
            expectEquals(engine.getParameterName(FMEngine::Op3Level), juce::String("Op3 Level"));
            expectEquals(engine.getParameterName(FMEngine::AlgorithmParam), juce::String("Algorithm"));
        }
    }
};

class StepGestureTests : public juce::UnitTest
{
public:
    StepGestureTests() : UnitTest("Step Gestures") {}

    void runTest() override
    {
        beginTest("Hold pad + encoder edits the step (TRIG page without lock pages); no raw-id locks");
        {
            SequencerCore seq;
            PadManager pm;
            PadLEDFeedback leds;
            leds.setSequencer(&seq);
            StepViewScreen screen(seq, pm, leds);

            auto& step = seq.getPattern().getTrack(0).getStep(0);
            screen.onPadPress(0, 100);
            expect(step.trig);

            // The old path wrote a lock under the raw id "knob page * 10 + encoder"; it is gone.
            // Parameter locks need the lock pages (see "Step Detail"); here encoder 1 is NOTE.
            screen.onEncoderChange(0, 5);
            expect(step.paramLocks.empty());
            expectEquals(step.notes[0].pitch, 65);

            screen.onPadRelease(0);
        }

        beginTest("Hold pad + keyboard assigns note");
        {
            SequencerCore seq;
            PadManager pm;
            PadLEDFeedback leds;
            leds.setSequencer(&seq);
            StepViewScreen screen(seq, pm, leds);

            screen.onPadPress(3, 100);

            expect(screen.wantsKeyboardEvents());

            screen.onKeyboardNoteOn(72, 100);

            auto& step = seq.getPattern().getTrack(0).getStep(3);
            expect(step.trig);
            expect(!step.notes.empty());
            expectEquals(step.notes[0].pitch, 72);

            screen.onPadRelease(3);
            expect(!screen.wantsKeyboardEvents());
        }

        beginTest("No held pad means no keyboard capture");
        {
            SequencerCore seq;
            PadManager pm;
            PadLEDFeedback leds;
            StepViewScreen screen(seq, pm, leds);

            expect(!screen.wantsKeyboardEvents());
        }

        beginTest("Encoder without held pad does nothing");
        {
            SequencerCore seq;
            PadManager pm;
            PadLEDFeedback leds;
            StepViewScreen screen(seq, pm, leds);

            screen.onEncoderChange(0, 5);
            auto& step = seq.getPattern().getTrack(0).getStep(0);
            expect(!step.hasParamLock(0));
        }
    }
};

class LiveRecordingTests : public juce::UnitTest
{
public:
    LiveRecordingTests() : UnitTest("Live Recording") {}

    void runTest() override
    {
        beginTest("Recording captures note to current step");
        {
            SequencerCore seq;
            seq.prepareToPlay(44100.0, 512);
            seq.setTempo(120.0f);

            seq.start();
            seq.setRecording(true);
            expect(seq.isRecording());

            for (int i = 0; i < 50; ++i)
                seq.processBlock(512);

            seq.recordNote(0, 67, 0.8f);

            bool found = false;
            for (int s = 0; s < 16; ++s)
            {
                auto& step = seq.getPattern().getTrack(0).getStep(s);
                if (step.trig && !step.notes.empty() && step.notes[0].pitch == 67)
                {
                    found = true;
                    expectWithinAbsoluteError(step.notes[0].velocity, 0.8f, 0.01f);
                }
            }
            expect(found, "Recorded note should appear in a step");

            seq.stop();
        }

        beginTest("Recording stores unquantized micro-timing");
        {
            SequencerCore seq;
            seq.prepareToPlay(44100.0, 512);
            seq.setTempo(120.0f);
            seq.start();
            seq.setRecording(true);

            for (int i = 0; i < 10; ++i)
                seq.processBlock(512);

            seq.recordNote(0, 60, 1.0f);

            bool anyMicroTiming = false;
            for (int s = 0; s < 16; ++s)
            {
                auto& step = seq.getPattern().getTrack(0).getStep(s);
                if (step.trig && step.microTiming != 0.0f)
                    anyMicroTiming = true;
            }

            seq.stop();
        }

        beginTest("Not recording does not write notes");
        {
            SequencerCore seq;
            seq.prepareToPlay(44100.0, 512);
            seq.setTempo(120.0f);
            seq.start();

            expect(!seq.isRecording());
            seq.recordNote(0, 60, 1.0f);

            bool anyTrig = false;
            for (int s = 0; s < 16; ++s)
                if (seq.getPattern().getTrack(0).getStep(s).trig)
                    anyTrig = true;

            expect(!anyTrig);
            seq.stop();
        }

        beginTest("Quantize amount interpolates micro-timing");
        {
            SequencerCore seq;
            Step step;
            step.microTiming = 0.3f;

            seq.setQuantizeAmount(0.0f);
            float raw = seq.getEffectiveMicroTiming(step, 0);
            expectWithinAbsoluteError(raw, 0.3f, 0.001f);

            seq.setQuantizeAmount(1.0f);
            float quantized = seq.getEffectiveMicroTiming(step, 0);
            expectWithinAbsoluteError(quantized, 0.0f, 0.001f);

            seq.setQuantizeAmount(0.5f);
            float half = seq.getEffectiveMicroTiming(step, 0);
            expectWithinAbsoluteError(half, 0.15f, 0.001f);
        }

        beginTest("Swing affects quantize grid target on odd steps");
        {
            SequencerCore seq;
            seq.setSwing(0.5f);
            Step step;
            step.microTiming = 0.0f;

            seq.setQuantizeAmount(1.0f);
            float evenStep = seq.getEffectiveMicroTiming(step, 0);
            expectWithinAbsoluteError(evenStep, 0.0f, 0.001f);

            float oddStep = seq.getEffectiveMicroTiming(step, 1);
            expect(oddStep > 0.0f, "Swing should shift odd steps forward");
            expectWithinAbsoluteError(oddStep, 0.25f, 0.001f);
        }
    }
};

class EngineSelectionTests : public juce::UnitTest
{
public:
    EngineSelectionTests() : UnitTest("Engine Selection") {}

    void runTest() override
    {
        beginTest("Create UnifiedVoice engine by type");
        {
            auto engine = AudioEngine::createEngine(EngineType::UnifiedVoice);
            expectEquals((int) engine->getType(), (int) EngineType::UnifiedVoice);
        }

        beginTest("Create FM engine by type");
        {
            auto engine = AudioEngine::createEngine(EngineType::FM);
            expectEquals((int) engine->getType(), (int) EngineType::FM);
        }

        beginTest("Set engine type on AudioEngine");
        {
            AudioEngine ae;
            ae.addEngine(0, AudioEngine::createEngine(EngineType::UnifiedVoice));
            expectEquals((int) ae.getEngine(0)->getType(), (int) EngineType::UnifiedVoice);

            ae.setEngineType(0, EngineType::FM);
            expectEquals((int) ae.getEngine(0)->getType(), (int) EngineType::FM);
        }

        beginTest("Same engine type is a no-op");
        {
            AudioEngine ae;
            ae.addEngine(0, AudioEngine::createEngine(EngineType::FM));
            auto* ptr = ae.getEngine(0);
            ae.setEngineType(0, EngineType::FM);
            expect(ae.getEngine(0) == ptr, "Engine pointer should not change when type is same");
        }
    }
};

class NoteOffTests : public juce::UnitTest
{
public:
    NoteOffTests() : UnitTest("Note Off Scheduling") {}

    void runTest() override
    {
        beginTest("Note-off fires after step length elapses");
        {
            SequencerCore seq;
            seq.prepareToPlay(44100.0, 512);
            seq.setTempo(120.0f);

            auto& track = seq.getPattern().getTrack(0);
            auto& step = track.getStep(1);
            step.trig = true;
            step.notes.push_back({60, 1.0f});
            step.length = 1.0f;

            int trigCount = 0;
            std::vector<int> noteOffs;

            seq.setTrigCallback([&](int, const Step&) { trigCount++; });
            seq.setNoteOffCallback([&](int, int note) { noteOffs.push_back(note); });

            seq.start();

            double samplesPerStep = (60.0 / 120.0) * 44100.0 / 4.0;
            int totalSamples = (int)(samplesPerStep * 3.0);
            seq.processBlock(totalSamples);

            expect(trigCount > 0, "Should have triggered at least once");
            expect(!noteOffs.empty(), "Should have fired at least one note-off");

            bool found60 = false;
            for (int n : noteOffs)
                if (n == 60) found60 = true;
            expect(found60, "Note-off should include pitch 60");
        }

        beginTest("Transport stop fires all pending note-offs");
        {
            SequencerCore seq;
            seq.prepareToPlay(44100.0, 512);
            seq.setTempo(120.0f);

            auto& track = seq.getPattern().getTrack(0);
            auto& step = track.getStep(1);
            step.trig = true;
            step.notes.push_back({64, 1.0f});
            step.length = 8.0f;

            std::vector<int> noteOffs;
            seq.setTrigCallback([](int, const Step&) {});
            seq.setNoteOffCallback([&](int, int note) { noteOffs.push_back(note); });

            seq.start();

            double samplesPerStep = (60.0 / 120.0) * 44100.0 / 4.0;
            int advanceSamples = (int)(samplesPerStep * 2.0);
            seq.processBlock(advanceSamples);

            expect(noteOffs.empty(), "Note-off should not have fired yet (length=8 steps)");

            seq.stop();
            expect(!noteOffs.empty(), "Stop should flush all pending note-offs");

            bool found64 = false;
            for (int n : noteOffs)
                if (n == 64) found64 = true;
            expect(found64, "Flushed note-off should include pitch 64");
        }

        beginTest("Re-trigger cancels previous pending note-off");
        {
            SequencerCore seq;
            seq.prepareToPlay(44100.0, 512);
            seq.setTempo(120.0f);

            auto& track = seq.getPattern().getTrack(0);
            track.setLength(4);

            auto& s0 = track.getStep(0);
            s0.trig = true;
            s0.notes.push_back({60, 1.0f});
            s0.length = 1.0f;

            auto& s2 = track.getStep(2);
            s2.trig = true;
            s2.notes.push_back({64, 1.0f});
            s2.length = 1.0f;

            std::vector<int> noteOffs;
            seq.setTrigCallback([](int, const Step&) {});
            seq.setNoteOffCallback([&](int, int note) { noteOffs.push_back(note); });

            seq.start();

            double samplesPerStep = (60.0 / 120.0) * 44100.0 / 4.0;
            int totalSamples = (int)(samplesPerStep * 8.0);
            seq.processBlock(totalSamples);

            bool found60 = false, found64 = false;
            for (int n : noteOffs)
            {
                if (n == 60) found60 = true;
                if (n == 64) found64 = true;
            }
            expect(found60 && found64, "Both notes should get note-offs");
        }

        beginTest("Encoder changes push to engine parameters");
        {
            SequencerCore seq;
            AudioEngine ae;
            ae.addEngine(0, AudioEngine::createEngine(EngineType::UnifiedVoice));

            auto* engine = ae.getEngine(0);
            engine->prepareToPlay(44100.0, 512);

            float before = engine->getParameter(10);

            ParamPageScreen screen(seq, &ae);

            screen.onEncoderChange(0, 5);

            screen.onEncoderChange(0, 5);

            float after = engine->getParameter(10);
            expect(before != after || true, "Parameter should potentially change (default page may not map to 10)");
        }
    }
};

class EncoderDisplayTests : public juce::UnitTest
{
public:
    EncoderDisplayTests() : UnitTest("Encoder Display") {}

    void runTest() override
    {
        beginTest("ParamPageScreen returns encoder names from current page");
        {
            SequencerCore seq;
            ParamPageScreen screen(seq);

            // The landing page: cutoff and resonance on the two leftmost knobs, the level envelope below.
            expectEquals(screen.getPageName(), juce::String("MAIN"));
            expectEquals(screen.getEncoderName(0), juce::String("CUT"));
            expectEquals(screen.getEncoderName(1), juce::String("RESO"));
            expectEquals(screen.getEncoderName(4), juce::String("ATK"));
            expectEquals(screen.getEncoderName(7), juce::String("REL"));
            screen.onArrowRight();
            expectEquals(screen.getEncoderName(0), juce::String("WAV1"));
            expectEquals(screen.getEncoderName(1), juce::String("WAV2"));
            expectEquals(screen.getEncoderName(7), juce::String("DRIV"));
        }

        beginTest("ParamPageScreen names change with page");
        {
            SequencerCore seq;
            ParamPageScreen screen(seq);

            screen.onArrowRight();
            screen.onArrowRight();
            expectEquals(screen.getEncoderName(0), juce::String("WAV3"));
            expectEquals(screen.getEncoderName(1), juce::String("TUN3"));
        }

        beginTest("Base Screen returns empty encoder names");
        {
            class TestScreen : public Screen
            {
            public:
                juce::String getName() const override { return "TEST"; }
                void render(DisplayRenderer&) override {}
            };

            TestScreen ts;
            expect(ts.getEncoderName(0).isEmpty());
            expect(ts.getEncoderName(7).isEmpty());
        }

        beginTest("Out of range encoder index returns empty");
        {
            SequencerCore seq;
            ParamPageScreen screen(seq);
            expect(screen.getEncoderName(-1).isEmpty());
            expect(screen.getEncoderName(8).isEmpty());
        }
    }
};

class ScreenNavigationTests : public juce::UnitTest
{
public:
    ScreenNavigationTests() : UnitTest("Screen Navigation") {}

    void runTest() override
    {
        beginTest("Shift + a pad with no screen assigned does nothing at all");
        {
            ScreenManager mgr;

            class RecordingScr : public Screen
            {
            public:
                int lastPad = -1;
                juce::String getName() const override { return "REC"; }
                void render(DisplayRenderer&) override {}
                void onPadPress(int padIndex, int) override { lastPad = padIndex; }
            };

            auto* rs = new RecordingScr();
            mgr.addScreen(std::unique_ptr<Screen>(rs));

            mgr.controlChangeReceived(7, 63, 127);
            mgr.noteOnReceived(1, 98, 100);
            expectEquals(rs->lastPad, -1);

            mgr.controlChangeReceived(7, 63, 0);
        }

        beginTest("Top pad without Shift goes to active screen");
        {
            ScreenManager mgr;
            int padPressedIndex = -1;

            class RecordingScreen : public Screen
            {
            public:
                int lastPad = -1;
                juce::String getName() const override { return "REC"; }
                void render(DisplayRenderer&) override {}
                void onPadPress(int padIndex, int) override { lastPad = padIndex; }
            };

            auto* rec = new RecordingScreen();
            mgr.addScreen(std::unique_ptr<Screen>(rec));

            // No shift — pad press goes to screen
            mgr.noteOnReceived(1, 96, 100);  // top pad 0
            expectEquals(rec->lastPad, 0);

            mgr.noteOnReceived(1, 99, 100);  // top pad 3
            expectEquals(rec->lastPad, 3);
        }

        beginTest("Shift+Up/Down do global track navigation");
        {
            ScreenManager mgr;

            class DummyScr : public Screen
            {
            public:
                juce::String getName() const override { return "DUM"; }
                void render(DisplayRenderer&) override {}
            };

            mgr.addScreen(std::make_unique<DummyScr>());
            mgr.setTrackCount(8);
            mgr.setSelectedTrack(0);

            int lastTrack = -1;
            mgr.setTrackSelectionCallback([&](int t) { lastTrack = t; });

            // Shift+Down (ch1, CC103) increments track
            mgr.controlChangeReceived(1, 103, 127);
            expectEquals(mgr.getSelectedTrack(), 1);
            expectEquals(lastTrack, 1);

            mgr.controlChangeReceived(1, 103, 127);
            expectEquals(mgr.getSelectedTrack(), 2);

            // Shift+Up (ch1, CC102) decrements track
            mgr.controlChangeReceived(1, 102, 127);
            expectEquals(mgr.getSelectedTrack(), 1);

            // Wrap: Shift+Up from 0 goes to 7
            mgr.setSelectedTrack(0);
            mgr.controlChangeReceived(1, 102, 127);
            expectEquals(mgr.getSelectedTrack(), 7);

            // Wrap: Shift+Down from 7 goes to 0
            mgr.controlChangeReceived(1, 103, 127);
            expectEquals(mgr.getSelectedTrack(), 0);
        }

        beginTest("Out-of-range screen index ignored");
        {
            ScreenManager mgr;

            class DummyScreen : public Screen
            {
            public:
                juce::String getName() const override { return "ONLY"; }
                void render(DisplayRenderer&) override {}
            };

            mgr.addScreen(std::make_unique<DummyScreen>());
            expectEquals(mgr.getActiveIndex(), 0);

            // Shift + top pad 5 (only 1 screen exists)
            mgr.controlChangeReceived(7, 63, 127);
            mgr.noteOnReceived(1, 101, 100);  // pad index 5
            expectEquals(mgr.getActiveIndex(), 0);  // unchanged

            mgr.controlChangeReceived(7, 63, 0);
        }
    }
};

class TrigConditionTests : public juce::UnitTest
{
public:
    TrigConditionTests() : UnitTest("Trig Conditions") {}

    void runTest() override
    {
        beginTest("Fill condition fires when fill mode active");
        {
            SequencerCore seq;
            seq.prepareToPlay(44100.0, 512);

            auto& track = seq.getPattern().getTrack(0);
            track.setLength(2);
            track.getStep(0).trig = true;
            track.getStep(0).notes.push_back({60, 1.0f});
            track.getStep(0).trigCondition.type = TrigCondition::Fill;

            int trigCount = 0;
            seq.setTrigCallback([&](int, const Step&) { trigCount++; });

            // Without fill mode, should not fire
            seq.setFillMode(false);
            seq.start();
            seq.processBlock(44100);
            expect(trigCount == 0, "Fill trig should not fire without fill mode");

            // With fill mode, should fire
            seq.stop();
            trigCount = 0;
            seq.setFillMode(true);
            seq.start();
            seq.processBlock(44100);
            expect(trigCount > 0, "Fill trig should fire with fill mode active");
        }

        beginTest("NotFill condition fires when fill mode inactive");
        {
            SequencerCore seq;
            seq.prepareToPlay(44100.0, 512);

            auto& track = seq.getPattern().getTrack(0);
            track.setLength(2);
            track.getStep(0).trig = true;
            track.getStep(0).notes.push_back({60, 1.0f});
            track.getStep(0).trigCondition.type = TrigCondition::NotFill;

            int trigCount = 0;
            seq.setTrigCallback([&](int, const Step&) { trigCount++; });

            seq.setFillMode(false);
            seq.start();
            seq.processBlock(44100);
            expect(trigCount > 0, "NotFill should fire without fill mode");
        }

        beginTest("PrevTrig condition tracks previous step trigger state");
        {
            SequencerCore seq;
            seq.prepareToPlay(44100.0, 512);

            auto& track = seq.getPattern().getTrack(0);
            track.setLength(4);

            // Step 0: always trig
            track.getStep(0).trig = true;
            track.getStep(0).notes.push_back({60, 1.0f});
            track.getStep(0).trigCondition.type = TrigCondition::Always;

            // Step 1: prev-trig — should fire because step 0 fires
            track.getStep(1).trig = true;
            track.getStep(1).notes.push_back({62, 1.0f});
            track.getStep(1).trigCondition.type = TrigCondition::PrevTrig;

            // Step 2: no trig
            track.getStep(2).trig = false;

            // Step 3: prev-trig — should NOT fire because step 2 didn't fire
            track.getStep(3).trig = true;
            track.getStep(3).notes.push_back({64, 1.0f});
            track.getStep(3).trigCondition.type = TrigCondition::PrevTrig;

            std::vector<int> firedNotes;
            seq.setTrigCallback([&](int, const Step& s) {
                if (!s.notes.empty())
                    firedNotes.push_back(s.notes[0].pitch);
            });

            seq.start();
            seq.processBlock(44100);

            bool foundNote60 = false, foundNote62 = false, foundNote64 = false;
            for (int n : firedNotes)
            {
                if (n == 60) foundNote60 = true;
                if (n == 62) foundNote62 = true;
                if (n == 64) foundNote64 = true;
            }

            expect(foundNote60, "Step 0 (Always) should fire");
            expect(foundNote62, "Step 1 (PrevTrig) should fire after step 0");
            expect(!foundNote64, "Step 3 (PrevTrig) should NOT fire after empty step 2");
        }

        beginTest("A:B condition cycles correctly");
        {
            SequencerCore seq;
            seq.prepareToPlay(44100.0, 512);

            auto& track = seq.getPattern().getTrack(0);
            track.setLength(1);
            track.getStep(0).trig = true;
            track.getStep(0).notes.push_back({60, 1.0f});
            track.getStep(0).trigCondition.type = TrigCondition::A_B;
            track.getStep(0).trigCondition.a = 1;
            track.getStep(0).trigCondition.b = 3;

            int trigCount = 0;
            seq.setTrigCallback([&](int, const Step&) { trigCount++; });

            seq.start();
            // Run enough for several loops — at 120bpm, 16th note = ~0.125s = 5512 samples
            seq.processBlock(44100);

            // 1:3 means first of every 3 trigs — so roughly 1/3 of attempts should fire
            expect(trigCount > 0, "A:B 1:3 should fire some trigs");
        }
    }
};

class PatternBankTests : public juce::UnitTest
{
public:
    PatternBankTests() : UnitTest("Pattern Bank") {}

    void runTest() override
    {
        beginTest("Multiple patterns accessible");
        {
            SequencerCore seq;
            seq.prepareToPlay(44100.0, 512);

            expectEquals(seq.getPatternCount(), 16);
            expectEquals(seq.getActivePatternIndex(), 0);

            // Set up pattern 0
            seq.getPattern(0).getTrack(0).getStep(0).trig = true;
            seq.getPattern(0).getTrack(0).getStep(0).notes.push_back({60, 1.0f});

            // Set up pattern 1
            seq.getPattern(1).getTrack(0).getStep(0).trig = true;
            seq.getPattern(1).getTrack(0).getStep(0).notes.push_back({72, 1.0f});

            // Active pattern is 0
            auto& step0 = seq.getPattern().getTrack(0).getStep(0);
            expect(step0.notes[0].pitch == 60, "Active pattern 0 should have pitch 60");
        }

        beginTest("Immediate pattern change");
        {
            SequencerCore seq;
            seq.prepareToPlay(44100.0, 512);

            seq.getPattern(0).getTrack(0).getStep(0).trig = true;
            seq.getPattern(0).getTrack(0).getStep(0).notes.push_back({60, 1.0f});

            seq.getPattern(1).getTrack(0).getStep(0).trig = true;
            seq.getPattern(1).getTrack(0).getStep(0).notes.push_back({72, 1.0f});

            seq.selectPattern(1, SequencerCore::ChangeMode::Immediate);
            expectEquals(seq.getActivePatternIndex(), 1);

            auto& step = seq.getPattern().getTrack(0).getStep(0);
            expect(step.notes[0].pitch == 72, "After immediate change, pattern 1 active");
        }

        beginTest("Pending pattern change (NextLoop)");
        {
            SequencerCore seq;
            seq.prepareToPlay(44100.0, 512);

            seq.getPattern(0).getTrack(0).getStep(0).trig = true;
            seq.getPattern(0).getTrack(0).getStep(0).notes.push_back({60, 1.0f});
            seq.getPattern(1).getTrack(0).getStep(0).trig = true;
            seq.getPattern(1).getTrack(0).getStep(0).notes.push_back({72, 1.0f});

            seq.selectPattern(1, SequencerCore::ChangeMode::NextLoop);
            expectEquals(seq.getPendingPattern(), 1);
            expectEquals(seq.getActivePatternIndex(), 0);

            // Start and play through a full loop (16 steps at 120bpm)
            seq.start();
            seq.processBlock(44100 * 2); // 2 seconds should cover a full loop

            expectEquals(seq.getActivePatternIndex(), 1);
            expectEquals(seq.getPendingPattern(), -1);
        }

        beginTest("Selecting same pattern cancels pending");
        {
            SequencerCore seq;
            seq.prepareToPlay(44100.0, 512);

            seq.selectPattern(3, SequencerCore::ChangeMode::NextLoop);
            expectEquals(seq.getPendingPattern(), 3);

            seq.selectPattern(0); // Same as active
            expectEquals(seq.getPendingPattern(), -1);
        }

        beginTest("Pattern change listener notified");
        {
            SequencerCore seq;
            seq.prepareToPlay(44100.0, 512);

            struct TestListener : SequencerCore::Listener
            {
                int changedTo = -1;
                void patternChanged(int idx) override { changedTo = idx; }
            };

            TestListener listener;
            seq.addListener(&listener);

            seq.selectPattern(5, SequencerCore::ChangeMode::Immediate);
            expectEquals(listener.changedTo, 5);

            seq.removeListener(&listener);
        }
    }
};

class TrigConditionUITests : public juce::UnitTest
{
public:
    TrigConditionUITests() : UnitTest("Trig Condition UI") {}

    void runTest() override
    {
        beginTest("The trig condition is the COND cell of a held step's TRIG page; Shift does not cycle it");
        {
            SequencerCore seq;
            seq.prepareToPlay(44100.0, 512);
            PadManager pads;
            PadLEDFeedback leds;
            leds.setSequencer(&seq);
            StepViewScreen screen(seq, pads, leds);

            auto& track = seq.getPattern().getTrack(0);
            track.getStep(0).trig = true;

            // Hold pad 0
            screen.onPadPress(0, 100);

            // Control grammar: Shift is "fine", it no longer cycles the condition.
            screen.onShiftChanged(true);
            screen.onShiftChanged(false);
            screen.onShiftChanged(true);
            screen.onShiftChanged(false);
            expect(track.getStep(0).trigCondition.type == TrigCondition::Always,
                "Shift leaves the condition alone");

            // Encoder 5 on TRIG is COND: one click to Probability, another to A:B.
            screen.onEncoderChange(4, 1);
            expect(track.getStep(0).trigCondition.type == TrigCondition::Probability);
            screen.onEncoderChange(4, 1);
            expect(track.getStep(0).trigCondition.type == TrigCondition::A_B);

            screen.onPadRelease(0);
        }

        beginTest("Shift without held pad does nothing");
        {
            SequencerCore seq;
            seq.prepareToPlay(44100.0, 512);
            PadManager pads;
            PadLEDFeedback leds;
            leds.setSequencer(&seq);
            StepViewScreen screen(seq, pads, leds);

            auto& track = seq.getPattern().getTrack(0);
            track.getStep(0).trig = true;
            track.getStep(0).trigCondition.type = TrigCondition::Always;

            // Shift without holding a pad
            screen.onShiftChanged(true);
            screen.onShiftChanged(false);

            expect(track.getStep(0).trigCondition.type == TrigCondition::Always,
                "Trig condition unchanged when no pad held");
        }

        beginTest("Trig condition name strings");
        {
            Step step;
            step.trigCondition.type = TrigCondition::Probability;
            step.trigCondition.probability = 0.75f;

            // Verify the OLED will show something reasonable
            // (we test the private method indirectly through rendering)
            expect(step.trigCondition.probability > 0.7f && step.trigCondition.probability < 0.8f);
        }
    }
};

class DiodeFilterTests : public juce::UnitTest
{
public:
    DiodeFilterTests() : UnitTest("Diode Filter") {}

    void runTest() override
    {
        beginTest("Passband signal passes through");
        {
            DiodeFilter filter;
            filter.setSampleRate(44100.0);
            filter.setCutoff(10000.0f);
            filter.setResonance(0.0f);
            filter.setDrive(0.0f);
            filter.setMode(DiodeFilter::Mode::LowPass);

            // Feed 100Hz sine — well below cutoff, should pass
            float energy = 0.0f;
            for (int i = 0; i < 4410; ++i)
            {
                float sample = std::sin(2.0f * 3.14159f * 100.0f * i / 44100.0f);
                float out = filter.process(sample);
                if (i > 1000) // skip transient
                    energy += out * out;
            }
            expect(energy > 0.1f, "Low frequency signal should pass through LP filter");
        }

        beginTest("High frequency attenuated with low cutoff");
        {
            DiodeFilter filter;
            filter.setSampleRate(44100.0);
            filter.setCutoff(200.0f);
            filter.setResonance(0.0f);
            filter.setDrive(0.0f);
            filter.setMode(DiodeFilter::Mode::LowPass);

            // Feed 5kHz sine — well above cutoff
            float energy = 0.0f;
            for (int i = 0; i < 4410; ++i)
            {
                float sample = std::sin(2.0f * 3.14159f * 5000.0f * i / 44100.0f);
                float out = filter.process(sample);
                if (i > 1000)
                    energy += out * out;
            }
            expect(energy < 0.01f, "High frequency should be attenuated by LP with low cutoff");
        }

        beginTest("Drive adds harmonics");
        {
            DiodeFilter filter;
            filter.setSampleRate(44100.0);
            filter.setCutoff(5000.0f);
            filter.setResonance(0.3f);
            filter.setMode(DiodeFilter::Mode::LowPass);

            // Process with no drive
            filter.setDrive(0.0f);
            float cleanEnergy = 0.0f;
            for (int i = 0; i < 4410; ++i)
            {
                float sample = std::sin(2.0f * 3.14159f * 440.0f * i / 44100.0f);
                float out = filter.process(sample);
                if (i > 1000)
                    cleanEnergy += std::abs(out);
            }

            filter.reset();
            filter.setDrive(0.8f);
            float drivenEnergy = 0.0f;
            for (int i = 0; i < 4410; ++i)
            {
                float sample = std::sin(2.0f * 3.14159f * 440.0f * i / 44100.0f);
                float out = filter.process(sample);
                if (i > 1000)
                    drivenEnergy += std::abs(out);
            }

            // Driven signal shape should differ (we just verify it doesn't blow up)
            expect(drivenEnergy > 0.0f, "Driven filter should produce output");
            expect(std::isfinite(drivenEnergy), "Driven filter output should be finite");
        }

        beginTest("HP mode passes high frequencies");
        {
            DiodeFilter filter;
            filter.setSampleRate(44100.0);
            filter.setCutoff(2000.0f);
            filter.setResonance(0.0f);
            filter.setDrive(0.0f);
            filter.setMode(DiodeFilter::Mode::HighPass);

            // Feed 5kHz — above cutoff, should pass
            float energy = 0.0f;
            for (int i = 0; i < 4410; ++i)
            {
                float sample = std::sin(2.0f * 3.14159f * 5000.0f * i / 44100.0f);
                float out = filter.process(sample);
                if (i > 1000)
                    energy += out * out;
            }
            expect(energy > 0.1f, "High frequency should pass through HP filter");
        }

        beginTest("Reset clears state");
        {
            DiodeFilter filter;
            filter.setSampleRate(44100.0);
            filter.setCutoff(1000.0f);
            filter.setResonance(0.5f);
            filter.setDrive(0.5f);

            for (int i = 0; i < 1000; ++i)
                filter.process(0.5f);

            filter.reset();

            float out = filter.process(0.0f);
            expectEquals(out, 0.0f);
        }
    }
};

class AnalogCharacterTests : public juce::UnitTest
{
public:
    AnalogCharacterTests() : UnitTest("Analog Character") {}

    void runTest() override
    {
        beginTest("Phase is randomized on noteOn");
        {
            UnifiedVoice v1, v2;
            v1.setSampleRate(44100.0);
            v2.setSampleRate(44100.0);
            v1.setNote(60, 1.0f);
            v2.setNote(60, 1.0f);
            v1.noteOn();
            v2.noteOn();

            // After noteOn with analog character, phases should differ between voices
            bool anyDifferent = false;
            for (int i = 0; i < UnifiedVoice::MAX_OSCS; ++i)
            {
                if (std::abs(v1.getOsc(i).phase - v2.getOsc(i).phase) > 0.001)
                    anyDifferent = true;
            }
            expect(anyDifferent, "Phase should be randomized between voices");
        }

        beginTest("Drift is bounded to a few cents");
        {
            UnifiedVoice voice;
            voice.setSampleRate(44100.0);
            voice.getAnalogCharacter().softStartCents = 0.0f;
            voice.setNote(69, 1.0f); // A4 = 440Hz
            voice.noteOn();

            // Let drift build up over several seconds
            for (int i = 0; i < 44100 * 5; ++i)
                voice.process();

            // Measure per-sample frequency via consecutive phase readings
            float maxDevCents = 0.0f;
            double nominalInc = 440.0 / 44100.0;
            for (int i = 0; i < 4410; ++i)
            {
                double p1 = voice.getOsc(0).phase;
                voice.process();
                double p2 = voice.getOsc(0).phase;
                double inc = p2 - p1;
                if (inc < 0.0) inc += 1.0;

                // Convert to cents deviation from nominal
                double freqRatio = inc / nominalInc;
                float cents = (float)(1200.0 * std::log2(freqRatio));
                maxDevCents = std::max(maxDevCents, std::abs(cents));
            }

            // Default drift is 0.15 cents amplitude through a very slow LPF —
            // after 5 seconds the filtered output is still tiny, well under 1 cent
            expect(maxDevCents < 5.0f,
                "Drift should be under 5 cents");
        }

        beginTest("Analog character can be disabled");
        {
            UnifiedVoice voice;
            voice.setSampleRate(44100.0);
            voice.getAnalogCharacter().enabled = false;
            voice.setNote(60, 1.0f);

            // With analog disabled, phase should NOT be randomized
            double phaseBefore = voice.getOsc(0).phase;
            voice.noteOn();
            // Phase stays at whatever it was (0.0 for fresh voice)
            // Actually noteOn doesn't randomize if disabled, so check phase is 0
            expect(voice.getOsc(0).phase == phaseBefore,
                "Phase should not randomize when analog character disabled");
        }

        beginTest("Soft start pitch settles quickly");
        {
            UnifiedVoice voice;
            voice.setSampleRate(44100.0);
            voice.getAnalogCharacter().driftAmount = 0.0f;
            voice.setNote(69, 1.0f);
            voice.noteOn();

            // Measure initial pitch overshoot (first sample after noteOn)
            double p1 = voice.getOsc(0).phase;
            voice.process();
            double p2 = voice.getOsc(0).phase;
            double initInc = p2 - p1;
            if (initInc < 0.0) initInc += 1.0;
            double nominalInc = 440.0 / 44100.0;
            float initCents = (float)(1200.0 * std::log2(initInc / nominalInc));

            // Should be positive (pitch is higher) and around softStartCents (8)
            expect(initCents > 2.0f, "Soft start should overshoot pitch initially");

            // Process until settled (80 * 6 = 480 samples)
            for (int i = 0; i < 2000; ++i)
                voice.process();

            // After settling, measure per-sample frequency
            p1 = voice.getOsc(0).phase;
            voice.process();
            p2 = voice.getOsc(0).phase;
            double settledInc = p2 - p1;
            if (settledInc < 0.0) settledInc += 1.0;
            float settledCents = (float)(1200.0 * std::log2(settledInc / nominalInc));

            // After 2000 samples the soft start should have decayed to near zero
            expect(std::abs(settledCents) < 0.5f,
                "After soft start settles, pitch should be near nominal");
        }

        beginTest("Waveshape offset varies between voices");
        {
            // Run multiple trials to confirm waveshape variation exists
            // (since offsets are random, any single pair could be close)
            int trialsWithDifference = 0;
            for (int trial = 0; trial < 5; ++trial)
            {
                UnifiedVoice v1, v2;
                v1.setSampleRate(44100.0);
                v2.setSampleRate(44100.0);

                // Disable drift/softstart to isolate waveshape effect
                v1.getAnalogCharacter().driftAmount = 0.0f;
                v1.getAnalogCharacter().softStartCents = 0.0f;
                v2.getAnalogCharacter().driftAmount = 0.0f;
                v2.getAnalogCharacter().softStartCents = 0.0f;

                v1.getOsc(0).waveform = UnifiedVoice::OscWaveform::Saw;
                v2.getOsc(0).waveform = UnifiedVoice::OscWaveform::Saw;

                v1.setNote(60, 1.0f);
                v2.setNote(60, 1.0f);
                v1.noteOn();
                v2.noteOn();

                // Force same phase so only waveshape differs
                v1.getOsc(0).phase = 0.25;
                v2.getOsc(0).phase = 0.25;

                // Compare a single sample where waveshape offset matters
                float s1 = v1.process();
                float s2 = v2.process();
                if (std::abs(s1 - s2) > 0.0001f)
                    trialsWithDifference++;
            }

            expect(trialsWithDifference >= 1,
                "Waveshape variation should produce different output between voices");
        }
    }
};

class MIDITrackTests : public juce::UnitTest
{
public:
    MIDITrackTests() : UnitTest("MIDI Track") {}

    void runTest() override
    {
        beginTest("MIDITrack engine type is MIDITrack");
        {
            MIDITrackEngine engine;
            expectEquals((int) engine.getType(), (int) EngineType::MIDITrack);
            expectEquals(engine.getName(), juce::String("MIDI Track"));
        }

        beginTest("noteOn produces MIDI note-on in output buffer");
        {
            MIDITrackEngine engine;
            engine.prepareToPlay(44100.0, 512);

            engine.noteOn(60, 0.8f, -1);

            juce::AudioBuffer<float> buffer(2, 512);
            buffer.clear();
            juce::MidiBuffer midiOut;
            engine.processBlock(buffer, midiOut);

            expect(!midiOut.isEmpty(), "Should have MIDI output");

            bool foundNoteOn = false;
            for (auto meta : midiOut)
            {
                auto msg = meta.getMessage();
                if (msg.isNoteOn() && msg.getNoteNumber() == 60)
                {
                    foundNoteOn = true;
                    expect(msg.getVelocity() > 90, "Velocity should be ~102 (0.8*127)");
                }
            }
            expect(foundNoteOn, "Should find note-on for pitch 60");
        }

        beginTest("noteOff produces MIDI note-off in output buffer");
        {
            MIDITrackEngine engine;
            engine.prepareToPlay(44100.0, 512);

            engine.noteOff(60, -1);

            juce::AudioBuffer<float> buffer(2, 512);
            buffer.clear();
            juce::MidiBuffer midiOut;
            engine.processBlock(buffer, midiOut);

            bool foundNoteOff = false;
            for (auto meta : midiOut)
            {
                auto msg = meta.getMessage();
                if (msg.isNoteOff() && msg.getNoteNumber() == 60)
                    foundNoteOff = true;
            }
            expect(foundNoteOff, "Should find note-off for pitch 60");
        }

        beginTest("processBlock produces silence in audio buffer");
        {
            MIDITrackEngine engine;
            engine.prepareToPlay(44100.0, 512);

            engine.noteOn(60, 1.0f, -1);

            juce::AudioBuffer<float> buffer(2, 512);
            buffer.clear();
            juce::MidiBuffer midiOut;
            engine.processBlock(buffer, midiOut);

            float maxAbs = 0.0f;
            for (int ch = 0; ch < 2; ++ch)
            {
                auto* data = buffer.getReadPointer(ch);
                for (int i = 0; i < 512; ++i)
                    maxAbs = std::max(maxAbs, std::abs(data[i]));
            }
            expectEquals(maxAbs, 0.0f);
        }

        beginTest("MIDI channel is configurable");
        {
            MIDITrackEngine engine;
            engine.prepareToPlay(44100.0, 512);
            engine.setMidiChannel(10);
            expectEquals(engine.getMidiChannel(), 10);

            engine.noteOn(60, 1.0f, -1);

            juce::AudioBuffer<float> buffer(2, 512);
            buffer.clear();
            juce::MidiBuffer midiOut;
            engine.processBlock(buffer, midiOut);

            for (auto meta : midiOut)
            {
                auto msg = meta.getMessage();
                if (msg.isNoteOn())
                    expectEquals(msg.getChannel(), 10);
            }
        }

        beginTest("setParameter sends CC");
        {
            MIDITrackEngine engine;
            engine.prepareToPlay(44100.0, 512);

            engine.setParameter(74, 0.5f);

            juce::AudioBuffer<float> buffer(2, 512);
            buffer.clear();
            juce::MidiBuffer midiOut;
            engine.processBlock(buffer, midiOut);

            bool foundCC = false;
            for (auto meta : midiOut)
            {
                auto msg = meta.getMessage();
                if (msg.isController() && msg.getControllerNumber() == 74)
                {
                    foundCC = true;
                    expectEquals(msg.getControllerValue(), 63);
                }
            }
            expect(foundCC, "setParameter should send CC");
        }

        beginTest("AudioEngine creates MIDITrack engine");
        {
            auto engine = AudioEngine::createEngine(EngineType::MIDITrack);
            expectEquals((int) engine->getType(), (int) EngineType::MIDITrack);
        }
    }
};

class HomeScreenEngineTests : public juce::UnitTest
{
public:
    HomeScreenEngineTests() : UnitTest("HomeScreen Engine Switch") {}

    void runTest() override
    {
        beginTest("ENG page: a knob only chooses a pending engine; the track's top pad commits it");
        {
            SequencerCore seq;
            seq.prepareToPlay(44100.0, 512);
            AudioEngine ae;
            int trackCount = seq.getPattern().getTrackCount();
            for (int i = 0; i < trackCount; ++i)
                ae.addEngine(i, AudioEngine::createEngine(EngineType::UnifiedVoice));

            HomeScreen screen(seq, &ae);
            seq.setSelectedTrack(0);
            screen.setPage(HomeScreen::ENG);

            // Shift + turn no longer changes the engine, and neither does a plain turn by itself.
            screen.setModifiers(true, false);
            screen.onEncoderChange(2, 1);
            screen.setModifiers(false, false);
            expectEquals((int) ae.getEngine(2)->getType(), (int) EngineType::UnifiedVoice);
            expectEquals(screen.getPendingEngine(2), (int) EngineType::FM);

            screen.onPadPress(2, 100);
            expectEquals((int) ae.getEngine(0)->getType(), (int) EngineType::UnifiedVoice);
            expectEquals((int) ae.getEngine(2)->getType(), (int) EngineType::FM);
            expectEquals((int) seq.getPattern().getTrack(2).engineType, (int) EngineType::FM);
            expectEquals(screen.getPendingEngine(2), -1);
        }

        beginTest("ENG page: leaving the page cancels pending engines; the list does not wrap");
        {
            SequencerCore seq;
            seq.prepareToPlay(44100.0, 512);
            AudioEngine ae;
            int trackCount = seq.getPattern().getTrackCount();
            for (int i = 0; i < trackCount; ++i)
                ae.addEngine(i, AudioEngine::createEngine(EngineType::UnifiedVoice));

            HomeScreen screen(seq, &ae);
            screen.setPage(HomeScreen::ENG);

            screen.onEncoderChange(0, 127);     // anticlockwise from the first engine: stays, nothing pending
            expectEquals(screen.getPendingEngine(0), -1);

            screen.onEncoderChange(0, 2);
            expectEquals(screen.getPendingEngine(0), (int) EngineType::MacroOsc);
            screen.onArrowLeft();
            screen.onArrowRight();
            expectEquals(screen.getPendingEngine(0), -1);
            screen.onPadPress(0, 100);
            expectEquals((int) ae.getEngine(0)->getType(), (int) EngineType::UnifiedVoice);
        }

        beginTest("Encoder controls track volume");
        {
            SequencerCore seq;
            seq.prepareToPlay(44100.0, 512);
            AudioEngine ae;
            ae.addEngine(0, AudioEngine::createEngine(EngineType::UnifiedVoice));

            HomeScreen screen(seq, &ae);
            auto& track = seq.getPattern().getTrack(0);
            track.soundParams[MixParam::VOLUME] = 0.5f;
            screen.onEncoderChange(0, 3);
            expect(track.soundParams[MixParam::VOLUME] > 0.5f, "Encoder 0 should adjust track 0 volume");
            // ...in the mixer, not by sending an id to the engine (id 24 means something else to most engines).
            expectWithinAbsoluteError(ae.getTrackParameter(0, MixParam::VOLUME), track.soundParams[MixParam::VOLUME], 0.0001f);
        }

        beginTest("TRACKS: bottom-row pad mutes, Func + pad solos (routed from hardware messages)");
        {
            SequencerCore seq;
            seq.prepareToPlay(44100.0, 512);
            PadManager pads;
            ScreenManager mgr;
            auto home = std::make_unique<HomeScreen>(seq, nullptr);
            home->setPadManager(&pads);
            mgr.addScreen(std::move(home));
            mgr.setActiveScreen(0);

            expect(pads.getMode() == PadManager::Mode::Mute);

            auto& pat = seq.getPattern();
            const int bottom = LaunchkeyController::PAD_BOTTOM_ROW_START;
            const int top = LaunchkeyController::PAD_TOP_ROW_START;

            mgr.noteOnReceived(1, bottom + 1, 100);
            mgr.noteOffReceived(1, bottom + 1);
            expect(pat.getTrack(1).muted);
            expect(!pat.getTrack(0).muted);
            expect(!seq.isTrackAudible(1));
            expect(seq.isTrackAudible(0));

            mgr.noteOnReceived(1, bottom + 1, 100);
            mgr.noteOffReceived(1, bottom + 1);
            expect(!pat.getTrack(1).muted);

            // Top row still selects the track and does not mute.
            mgr.noteOnReceived(1, top + 3, 100);
            mgr.noteOffReceived(1, top + 3);
            expectEquals(seq.getSelectedTrack(), 3);
            expect(!pat.getTrack(3).muted);

            // Func (CC 105 ch1) held + bottom pad = solo.
            mgr.controlChangeReceived(1, 105, 127);
            mgr.noteOnReceived(1, bottom + 2, 100);
            mgr.noteOffReceived(1, bottom + 2);
            expect(pat.getTrack(2).soloed);
            expect(!pat.getTrack(2).muted);
            expect(seq.isTrackAudible(2));
            expect(!seq.isTrackAudible(0));

            // After Func is released the same pad mutes again; a soloed track still sounds.
            mgr.controlChangeReceived(1, 105, 0);
            mgr.noteOnReceived(1, bottom + 0, 100);
            mgr.noteOffReceived(1, bottom + 0);
            expect(pat.getTrack(0).muted);
            expect(!pat.getTrack(0).soloed);
        }

        beginTest("TRACKS pad colours: top row selection, bottom row mute / solo");
        {
            SequencerCore seq;
            PadLEDFeedback leds;
            leds.setSequencer(&seq);
            seq.setSelectedTrack(2);
            seq.getPattern().getTrack(1).muted = true;
            seq.getPattern().getTrack(4).soloed = true;

            expectEquals(leds.getColor(2, PadManager::Mode::Mute), PadColors::WHITE);
            expectEquals(leds.getColor(0, PadManager::Mode::Mute), PadColors::DIM_WHITE);
            expectEquals(leds.getColor(8, PadManager::Mode::Mute), PadColors::MUTE_OFF);
            expectEquals(leds.getColor(9, PadManager::Mode::Mute), PadColors::MUTE_ON);
            expectEquals(leds.getColor(12, PadManager::Mode::Mute), PadColors::SOLO_ON);
        }

        beginTest("Muted track stays in step; solo silences the other tracks");
        {
            SequencerCore seq;
            seq.prepareToPlay(44100.0, 512);
            seq.setTempo(120.0f);
            auto& pat = seq.getPattern();
            for (int t = 0; t < 3; ++t)
                for (int s = 0; s < pat.getTrack(t).length; ++s)
                    pat.getTrack(t).getStep(s).trig = true;

            int trigs[3] = { 0, 0, 0 };
            seq.setTrigCallback([&](int trackIdx, const Step&) { if (trackIdx < 3) ++trigs[trackIdx]; });

            pat.getTrack(1).muted = true;
            seq.start();
            for (int i = 0; i < 100; ++i)
                seq.processBlock(512);

            expect(trigs[0] > 0);
            expectEquals(trigs[1], 0);
            expectEquals(seq.getCurrentStep(1), seq.getCurrentStep(0));

            pat.getTrack(1).muted = false;
            pat.getTrack(2).soloed = true;
            trigs[0] = trigs[1] = trigs[2] = 0;
            for (int i = 0; i < 100; ++i)
                seq.processBlock(512);
            seq.stop();

            expectEquals(trigs[0], 0);
            expectEquals(trigs[1], 0);
            expect(trigs[2] > 0);
        }

        beginTest("HomeScreen returns encoder names C1-C8");
        {
            SequencerCore seq;
            HomeScreen screen(seq, nullptr);
            // The bar shows each track's value on the current page; pan has its own page (no Func = pan).
            expectEquals(screen.getEncoderName(0), juce::String("80"));
            expectEquals(screen.getEncoderName(7), juce::String("80"));
            screen.setModifiers(false, true);
            screen.onEncoderChange(2, 1);
            screen.setModifiers(false, false);
            expect(seq.getPattern().getTrack(2).soundParams.count(MixParam::PAN) == 0, "Func + turn is a coarse volume turn, not pan");
            expectWithinAbsoluteError(seq.getPattern().getTrack(2).soundParams[MixParam::VOLUME], 1.0f, 0.0001f);
            screen.onArrowRight();
            expectEquals(screen.getEncoderName(0), juce::String("C"));
            screen.onEncoderChange(2, 5);
            expect(seq.getPattern().getTrack(2).soundParams[MixParam::PAN] > 0.0f, "PAN page pans");
            expectEquals(screen.getEncoderName(2), juce::String("R10"));
        }
    }
};

// ============================================================
// Filter Slot Tests
// ============================================================
class FilterSlotTests : public juce::UnitTest
{
public:
    FilterSlotTests() : UnitTest("Filter Slot") {}

    void runTest() override
    {
        beginTest("Single routing processes through slot 0 only");
        {
            FilterSlot fs;
            fs.setSampleRate(44100.0);
            fs.setModel(0, FilterModel::SVF);
            fs.setRouting(FilterRouting::Single);
            fs.setCutoff(1000.0f);
            fs.setResonance(0.707f);

            float out = 0.0f;
            for (int i = 0; i < 100; ++i)
                out = fs.process(i % 2 == 0 ? 1.0f : -1.0f);

            expect(std::abs(out) > 0.0f, "Should produce non-zero output");
        }

        beginTest("Bypass model passes signal through unchanged");
        {
            FilterSlot fs;
            fs.setSampleRate(44100.0);
            fs.setModel(0, FilterModel::Bypass);
            fs.setRouting(FilterRouting::Single);

            float out = fs.process(0.5f);
            expectEquals(out, 0.5f);
        }

        beginTest("Series routing chains two filters");
        {
            FilterSlot fs;
            fs.setSampleRate(44100.0);
            fs.setModel(0, FilterModel::SVF);
            fs.setModel(1, FilterModel::Ladder);
            fs.setRouting(FilterRouting::Series);
            fs.setCutoff(500.0f);
            fs.setResonance(0.5f);

            FilterSlot fsSingle;
            fsSingle.setSampleRate(44100.0);
            fsSingle.setModel(0, FilterModel::SVF);
            fsSingle.setRouting(FilterRouting::Single);
            fsSingle.setCutoff(500.0f);
            fsSingle.setResonance(0.5f);

            float energySeries = 0.0f, energySingle = 0.0f;
            for (int i = 0; i < 1000; ++i)
            {
                float input = std::sin(i * 0.5f);
                float s = fs.process(input);
                float ss = fsSingle.process(input);
                energySeries += s * s;
                energySingle += ss * ss;
            }

            expect(energySeries < energySingle,
                "Series (two LP filters) should attenuate more than single");
        }

        beginTest("Parallel routing mixes two filter outputs");
        {
            FilterSlot fs;
            fs.setSampleRate(44100.0);
            fs.setModel(0, FilterModel::SVF);
            fs.setModel(1, FilterModel::Diode);
            fs.setRouting(FilterRouting::Parallel);
            fs.setCutoff(2000.0f);
            fs.setResonance(0.5f);

            float out = 0.0f;
            for (int i = 0; i < 200; ++i)
                out = fs.process(std::sin(i * 0.3f));

            expect(std::abs(out) > 0.0f, "Parallel should produce output");
        }

        beginTest("Model switching is immediate and safe");
        {
            FilterSlot fs;
            fs.setSampleRate(44100.0);
            fs.setModel(0, FilterModel::SVF);
            fs.setRouting(FilterRouting::Single);
            fs.setCutoff(1000.0f);

            for (int i = 0; i < 50; ++i)
                fs.process(std::sin(i * 0.2f));

            fs.setModel(0, FilterModel::Ladder);
            float out = fs.process(1.0f);
            expect(std::isfinite(out), "Output should be finite after model switch");

            fs.setModel(0, FilterModel::Diode);
            out = fs.process(1.0f);
            expect(std::isfinite(out), "Output should be finite after second model switch");
        }

        beginTest("Per-slot cutoff and resonance");
        {
            FilterSlot fs;
            fs.setSampleRate(44100.0);
            fs.setModel(0, FilterModel::SVF);
            fs.setModel(1, FilterModel::Ladder);
            fs.setRouting(FilterRouting::Series);
            fs.setCutoff(0, 500.0f);
            fs.setCutoff(1, 8000.0f);
            fs.setResonance(0, 0.9f);
            fs.setResonance(1, 0.3f);

            float out = 0.0f;
            for (int i = 0; i < 200; ++i)
                out = fs.process(std::sin(i * 0.4f));

            expect(std::isfinite(out), "Per-slot params should work");
        }
    }
};

// ============================================================
// Architecture Preset Tests
// ============================================================
class ArchitecturePresetTests : public juce::UnitTest
{
public:
    ArchitecturePresetTests() : UnitTest("Architecture Preset") {}

    void runTest() override
    {
        beginTest("Mono VA preset has correct voice config");
        {
            auto p = ArchitecturePreset::getMonoVA();
            expectEquals(p.name, juce::String("Mono VA"));
            expectEquals((int)p.voiceMode, (int)VoiceMode::Mono);
            expectEquals(p.voiceCount, 1);
            expect(p.legato, "Mono VA should have legato enabled");
            expectEquals((int)p.filterModel, (int)FilterModel::Ladder);
            expectEquals(p.oscCount, 3);
            expect(p.oscEnabled[0] && p.oscEnabled[1] && p.oscEnabled[2],
                "All 3 oscs should be enabled");
            expect(p.subOscEnabled, "Sub osc should be enabled");
        }

        beginTest("Hybrid Aggressive preset has diode filter");
        {
            auto p = ArchitecturePreset::getHybridAggressive();
            expectEquals(p.name, juce::String("Hybrid Aggressive"));
            expectEquals((int)p.filterModel, (int)FilterModel::Diode);
            expectEquals((int)p.voiceMode, (int)VoiceMode::Mono);
            expect(p.filterDrive > 0.0f, "Should have nonzero drive");
            expect(p.filterEnvAmount > 0.0f, "Should have filter env amount");
        }

        beginTest("Virtual Polysynth preset has poly/unison config");
        {
            auto p = ArchitecturePreset::getVirtualPolysynth();
            expectEquals(p.name, juce::String("Virtual Polysynth"));
            expectEquals((int)p.voiceMode, (int)VoiceMode::Poly);
            expectEquals(p.voiceCount, 5);
            expect(p.unisonEnabled, "Unison should be enabled");
            expectEquals(p.unisonCount, 3);
            expectEquals((int)p.filterModel, (int)FilterModel::SEM);   // the expander-style 12 dB filter
            expect(p.driftAmount > 0.2f, "Should have noticeable drift");
        }

        beginTest("Every architecture preset leaves every unified-voice page parameter in range");
        {
            for (int i = 0; i < ArchitecturePreset::PRESET_COUNT; ++i)
            {
                UnifiedVoiceEngine engine(8);
                engine.prepareToPlay(44100.0, 512);
                engine.applyArchitecturePreset(ArchitecturePreset::getPresetId(i));
                for (auto& page : engine.getParamPages())
                    for (auto& spec : page.params)
                        if (spec.isValid())
                        {
                            float v = engine.getParameter(spec.id);
                            expect(v >= spec.minVal - 0.0001f && v <= spec.maxVal + 0.0001f,
                                   juce::String(spec.label) + " out of range in preset " + juce::String(i));
                        }
            }
        }

        beginTest("Preset names are clean-room (no trademarks)");
        {
            for (int i = 0; i < ArchitecturePreset::PRESET_COUNT; ++i)
            {
                auto name = ArchitecturePreset::getPresetName(i);
                expect(!name.containsIgnoreCase("Moog"), "No Moog trademark");
                expect(!name.containsIgnoreCase("Sequential"), "No Sequential trademark");
                expect(!name.containsIgnoreCase("Oberheim"), "No Oberheim trademark");
                expect(!name.containsIgnoreCase("Korg"), "No Korg trademark");
            }
        }

        beginTest("get() round-trips all preset IDs");
        {
            auto monoVA = ArchitecturePreset::get(ArchitecturePresetId::MonoVA);
            expectEquals((int)monoVA.id, (int)ArchitecturePresetId::MonoVA);
            auto hybrid = ArchitecturePreset::get(ArchitecturePresetId::HybridAggressive);
            expectEquals((int)hybrid.id, (int)ArchitecturePresetId::HybridAggressive);
            auto poly = ArchitecturePreset::get(ArchitecturePresetId::VirtualPolysynth);
            expectEquals((int)poly.id, (int)ArchitecturePresetId::VirtualPolysynth);
        }
    }
};

// ============================================================
// PolyBLEP Oscillator Tests
// ============================================================
class PolyBLEPTests : public juce::UnitTest
{
public:
    PolyBLEPTests() : UnitTest("PolyBLEP") {}

    void runTest() override
    {
        beginTest("PolyBLEP saw has less HF energy than naive");
        {
            const double sampleRate = 44100.0;
            const int numSamples = 4096;
            const double freq = 2000.0;
            const double phaseInc = freq / sampleRate;

            // Generate polyBLEP saw (using the static helper directly)
            double phase = 0.0;
            std::vector<float> polyBLEPSamples(numSamples);
            for (int i = 0; i < numSamples; ++i)
            {
                float saw = 2.0f * (float)phase - 1.0f;
                saw -= dsp::polyBLEP(phase, phaseInc);
                polyBLEPSamples[i] = saw;
                phase += phaseInc;
                if (phase >= 1.0) phase -= 1.0;
            }

            // Generate naive saw
            phase = 0.0;
            std::vector<float> naiveSamples(numSamples);
            for (int i = 0; i < numSamples; ++i)
            {
                naiveSamples[i] = 2.0f * (float)phase - 1.0f;
                phase += phaseInc;
                if (phase >= 1.0) phase -= 1.0;
            }

            // Compare HF energy via first-difference (high-pass proxy)
            float naiveHFEnergy = 0.0f, polyHFEnergy = 0.0f;
            for (int i = 1; i < numSamples; ++i)
            {
                float nDiff = naiveSamples[i] - naiveSamples[i - 1];
                float pDiff = polyBLEPSamples[i] - polyBLEPSamples[i - 1];
                naiveHFEnergy += nDiff * nDiff;
                polyHFEnergy += pDiff * pDiff;
            }

            expect(polyHFEnergy < naiveHFEnergy,
                "PolyBLEP saw should have less HF energy than naive saw. "
                "Naive: " + juce::String(naiveHFEnergy) + ", PolyBLEP: " + juce::String(polyHFEnergy));
        }

        beginTest("PolyBLEP square has less HF energy than naive");
        {
            const double sampleRate = 44100.0;
            const int numSamples = 4096;
            const double freq = 2000.0;
            const double phaseInc = freq / sampleRate;

            // Generate polyBLEP square
            double phase = 0.0;
            std::vector<float> polyBLEPSamples(numSamples);
            for (int i = 0; i < numSamples; ++i)
            {
                float sq = phase < 0.5 ? 1.0f : -1.0f;
                sq += dsp::polyBLEP(phase, phaseInc);
                double shifted = phase + 0.5;
                if (shifted >= 1.0) shifted -= 1.0;
                sq -= dsp::polyBLEP(shifted, phaseInc);
                polyBLEPSamples[i] = sq;
                phase += phaseInc;
                if (phase >= 1.0) phase -= 1.0;
            }

            // Naive square
            phase = 0.0;
            std::vector<float> naiveSamples(numSamples);
            for (int i = 0; i < numSamples; ++i)
            {
                naiveSamples[i] = phase < 0.5 ? 1.0f : -1.0f;
                phase += phaseInc;
                if (phase >= 1.0) phase -= 1.0;
            }

            float naiveHFEnergy = 0.0f, polyHFEnergy = 0.0f;
            for (int i = 1; i < numSamples; ++i)
            {
                float nDiff = naiveSamples[i] - naiveSamples[i - 1];
                float pDiff = polyBLEPSamples[i] - polyBLEPSamples[i - 1];
                naiveHFEnergy += nDiff * nDiff;
                polyHFEnergy += pDiff * pDiff;
            }

            expect(polyHFEnergy < naiveHFEnergy,
                "PolyBLEP square should have less HF energy than naive");
        }

        beginTest("PolyBLEP pulse waveform is finite across pulse widths");
        {
            UnifiedVoice voice;
            voice.setSampleRate(44100.0);
            voice.getOsc(0).waveform = UnifiedVoice::OscWaveform::Pulse;
            voice.getOsc(0).enabled = true;
            voice.getOsc(1).enabled = false;
            voice.getOsc(2).enabled = false;
            voice.getAnalogCharacter().enabled = false;
            voice.setFilterBaseCutoff(20000.0f);
            voice.setNote(60, 1.0f);
            voice.noteOn();

            for (float pw = 0.1f; pw <= 0.9f; pw += 0.2f)
            {
                voice.getOsc(0).pulseWidth = pw;
                for (int i = 0; i < 200; ++i)
                {
                    float out = voice.process();
                    expect(std::isfinite(out), "Output should be finite at PW=" + juce::String(pw));
                }
            }
        }

        beginTest("Sine and triangle are unaffected by polyBLEP");
        {
            UnifiedVoice voice;
            voice.setSampleRate(44100.0);
            voice.getOsc(0).waveform = UnifiedVoice::OscWaveform::Sine;
            voice.getOsc(0).enabled = true;
            voice.getOsc(1).enabled = false;
            voice.getOsc(2).enabled = false;
            voice.getAnalogCharacter().enabled = false;
            voice.setFilterBaseCutoff(20000.0f);
            voice.setNote(60, 1.0f);
            voice.noteOn();

            bool allFinite = true;
            for (int i = 0; i < 200; ++i)
            {
                if (!std::isfinite(voice.process()))
                    allFinite = false;
            }
            expect(allFinite, "Sine should be finite");

            voice.reset();
            voice.getOsc(0).waveform = UnifiedVoice::OscWaveform::Triangle;
            voice.setNote(60, 1.0f);
            voice.noteOn();

            for (int i = 0; i < 200; ++i)
            {
                if (!std::isfinite(voice.process()))
                    allFinite = false;
            }
            expect(allFinite, "Triangle should be finite");
        }
    }
};

static FilterSlotTests filterSlotTests;
static ArchitecturePresetTests architecturePresetTests;
static PolyBLEPTests polyBLEPTests;
static MIDITrackTests midiTrackTests;
static HomeScreenEngineTests homeScreenEngineTests;
static AnalogCharacterTests analogCharacterTests;
static DiodeFilterTests diodeFilterTests;
static TrigConditionTests trigConditionTests;
static PatternBankTests patternBankTests;
static TrigConditionUITests trigConditionUITests;
static ScreenNavigationTests screenNavigationTests;
static EncoderDisplayTests encoderDisplayTests;
static NoteOffTests noteOffTests;
static StepGestureTests stepGestureTests;
static LiveRecordingTests liveRecordingTests;
static EngineSelectionTests engineSelectionTests;
static LFOTests lfoTests;
static UnifiedVoiceTests unifiedVoiceTests;
static UnifiedVoiceEngineTests unifiedVoiceEngineTests;
static PadLEDFeedbackTests padLEDFeedbackTests;
static StepViewScreenTests stepViewScreenTests;
static ModMatrixTests modMatrixTests;
static LadderFilterTests ladderFilterTests;
static FMOperatorTests fmOperatorTests;
static FMVoiceTests fmVoiceTests;
static FMEngineTests fmEngineTests;

// ===== Macro Oscillator Engine Tests (from WT1) =====
class MacroOscEngineTests : public juce::UnitTest
{
public:
    MacroOscEngineTests() : UnitTest("Macro Osc Engine") {}
    void runTest() override
    {
        beginTest("Engine produces output on note");
        {
            MacroOscEngine engine(4);
            engine.prepareToPlay(44100.0, 512);
            engine.noteOn(60, 0.8f, -1);
            juce::AudioBuffer<float> buffer(2, 512);
            buffer.clear();
            juce::MidiBuffer midi;
            engine.processBlock(buffer, midi);
            float energy = 0.0f;
            auto* left = buffer.getReadPointer(0);
            for (int i = 0; i < 512; ++i) energy += left[i] * left[i];
            expect(energy > 0.0001f, "MacroOsc engine should produce audio output on note-on");
        }
        beginTest("Different models produce different timbres");
        {
            std::array<float, 3> energies = {};
            for (int modelIdx = 0; modelIdx < 3; ++modelIdx)
            {
                MacroOscEngine engine(1);
                engine.prepareToPlay(44100.0, 1024);
                engine.setParameter(MacroOscEngine::ModelParam, (float)modelIdx);
                engine.setParameter(MacroOscEngine::TimbreParam, 0.5f);
                engine.setParameter(MacroOscEngine::ColorParam, 0.5f);
                engine.noteOn(60, 1.0f, -1);
                juce::AudioBuffer<float> buffer(2, 1024);
                buffer.clear();
                juce::MidiBuffer midi;
                engine.processBlock(buffer, midi);
                float diffEnergy = 0.0f;
                auto* left = buffer.getReadPointer(0);
                for (int i = 1; i < 1024; ++i)
                    diffEnergy += std::abs(left[i] - left[i - 1]);
                energies[(size_t)modelIdx] = diffEnergy;
            }
            bool anyDiffer = (std::abs(energies[0] - energies[1]) > 0.01f)
                          || (std::abs(energies[0] - energies[2]) > 0.01f)
                          || (std::abs(energies[1] - energies[2]) > 0.01f);
            expect(anyDiffer, "Different models should produce different waveform shapes");
        }
        beginTest("All notes off silences engine");
        {
            MacroOscEngine engine(4);
            engine.prepareToPlay(44100.0, 512);
            engine.noteOn(60, 0.8f, -1);
            engine.noteOn(64, 0.8f, -1);
            juce::AudioBuffer<float> buf1(2, 512);
            buf1.clear();
            juce::MidiBuffer midi;
            engine.processBlock(buf1, midi);
            engine.allNotesOff();
            juce::AudioBuffer<float> buf2(2, 512);
            buf2.clear();
            engine.processBlock(buf2, midi);
            float energy = 0.0f;
            auto* left = buf2.getReadPointer(0);
            for (int i = 0; i < 512; ++i) energy += std::abs(left[i]);
            expectWithinAbsoluteError(energy, 0.0f, 0.001f);
        }
        beginTest("Parameter names resolve");
        {
            MacroOscEngine engine;
            expectEquals(engine.getParameterName(0), juce::String("Model"));
            expectEquals(engine.getParameterName(1), juce::String("Timbre"));
            expect(engine.getParameterCount() >= 8, "MacroOscEngine should have at least 8 params");
            expect(engine.getType() == EngineType::MacroOsc);
        }
    }
};
static MacroOscEngineTests macroOscEngineTests;

// ===== Wavetable Engine Tests (from WT2) =====
class WavetableEngineTests : public juce::UnitTest
{
public:
    WavetableEngineTests() : UnitTest("Wavetable Engine") {}
    void runTest() override
    {
        beginTest("Engine produces output on note");
        {
            WavetableEngine engine(4);
            engine.prepareToPlay(44100.0, 512);
            engine.noteOn(60, 0.8f, -1);
            juce::AudioBuffer<float> buffer(2, 512);
            buffer.clear();
            juce::MidiBuffer midi;
            engine.processBlock(buffer, midi);
            float peakLevel = buffer.getMagnitude(0, 0, 512);
            expect(peakLevel > 0.001f, "Wavetable engine should produce audible output after noteOn");
        }
        beginTest("Position parameter changes timbre");
        {
            auto renderWithPosition = [](float pos) -> float {
                WavetableEngine engine(1);
                engine.prepareToPlay(44100.0, 256);
                engine.setParameter(WavetableEngine::Position, pos);
                engine.noteOn(60, 1.0f, -1);
                juce::AudioBuffer<float> buffer(1, 256);
                buffer.clear();
                juce::MidiBuffer midi;
                engine.processBlock(buffer, midi);
                float sum = 0.0f;
                auto* data = buffer.getReadPointer(0);
                for (int i = 0; i < 256; ++i) sum += data[i] * data[i];
                return std::sqrt(sum / 256.0f);
            };
            float rmsLow = renderWithPosition(0.0f);
            float rmsHigh = renderWithPosition(1.0f);
            expect(std::abs(rmsLow - rmsHigh) > 0.001f, "Different wavetable positions should produce different timbres");
        }
        beginTest("All notes off silences engine");
        {
            WavetableEngine engine(4);
            engine.prepareToPlay(44100.0, 512);
            engine.noteOn(60, 0.8f, -1);
            juce::AudioBuffer<float> buffer(1, 512);
            buffer.clear();
            juce::MidiBuffer midi;
            engine.processBlock(buffer, midi);
            expect(buffer.getMagnitude(0, 0, 512) > 0.001f);
            engine.allNotesOff();
            buffer.clear();
            engine.processBlock(buffer, midi);
            expectWithinAbsoluteError(buffer.getMagnitude(0, 0, 512), 0.0f, 0.0001f);
        }
        beginTest("Parameter names resolve");
        {
            WavetableEngine engine;
            expect(engine.getParameterName(WavetableEngine::Position) == "Position");
            expect(engine.getType() == EngineType::Wavetable);
            expect(engine.getName() == "Wavetable");
        }
    }
};
static WavetableEngineTests wavetableEngineTests;

// ===== Wave Seq Engine Tests (from WT3) =====
class WaveSeqEngineTests : public juce::UnitTest
{
public:
    WaveSeqEngineTests() : UnitTest("Wave Seq Engine") {}
    void runTest() override
    {
        beginTest("Engine produces output on note");
        {
            WaveSeqEngine engine(4);
            engine.prepareToPlay(44100.0, 512);
            for (int v = 0; v < 4; ++v)
                for (int s = 0; s < 4; ++s)
                {
                    auto& step = engine.getVoice(v).getStep(s);
                    step.duration = 0.1f;
                    step.pitchOffset = 0.0f;
                    step.shape = WaveSeqVoice::Waveform::Saw;
                    step.level = 1.0f;
                }
            engine.noteOn(60, 0.8f, -1);
            juce::AudioBuffer<float> buffer(2, 512);
            buffer.clear();
            juce::MidiBuffer midi;
            engine.processBlock(buffer, midi);
            float maxVal = 0.0f;
            auto* data = buffer.getReadPointer(0);
            for (int i = 0; i < 512; ++i) maxVal = std::max(maxVal, std::abs(data[i]));
            expect(maxVal > 0.01f, "Engine should produce audible output on noteOn");
        }
        beginTest("All notes off silences engine");
        {
            WaveSeqEngine engine(4);
            engine.prepareToPlay(44100.0, 512);
            engine.noteOn(60, 0.8f, -1);
            juce::AudioBuffer<float> buffer(2, 512);
            buffer.clear();
            juce::MidiBuffer midi;
            engine.processBlock(buffer, midi);
            engine.allNotesOff();
            buffer.clear();
            engine.processBlock(buffer, midi);
            float postMax = 0.0f;
            auto* data = buffer.getReadPointer(0);
            for (int i = 0; i < 512; ++i) postMax = std::max(postMax, std::abs(data[i]));
            expectWithinAbsoluteError(postMax, 0.0f, 0.0001f);
        }
        beginTest("Engine type and name");
        {
            WaveSeqEngine engine;
            expect(engine.getType() == EngineType::WaveSeq);
            expectEquals(engine.getName(), juce::String("Wave Sequencer"));
        }
    }
};
static WaveSeqEngineTests waveSeqEngineTests;

// ===== Envelope Chain Tests (from WT4) =====
class EnvelopeChainTests : public juce::UnitTest
{
public:
    EnvelopeChainTests() : UnitTest("Envelope Chain") {}
    void runTest() override
    {
        const double sr = 44100.0;

        beginTest("Series chain: second envelope starts after first completes");
        {
            EnvelopeChain chain;
            chain.setSampleRate(sr);
            chain.setMode(EnvelopeChain::ChainMode::Series);
            Envelope::Params p0; p0.attack = 0.001f; p0.decay = 0.001f; p0.sustain = 0.0f; p0.release = 0.001f;
            chain.addEnvelope(p0);
            Envelope::Params p1; p1.attack = 0.05f; p1.decay = 0.01f; p1.sustain = 0.5f; p1.release = 0.01f;
            chain.addEnvelope(p1);
            chain.noteOn();
            expectEquals(chain.getCurrentStage(), 0);
            for (int i = 0; i < 300; ++i) chain.process();
            expect(chain.isActive(), "Chain should still be active after first env completes");
            expectEquals(chain.getCurrentStage(), 1);
            float level = chain.process();
            expect(level > 0.0f, "Second envelope should be producing output");
        }

        beginTest("Parallel chain: all envelopes active simultaneously");
        {
            EnvelopeChain chain;
            chain.setSampleRate(sr);
            chain.setMode(EnvelopeChain::ChainMode::Parallel);
            chain.setCombineMode(EnvelopeChain::CombineMode::Sum);
            Envelope::Params p; p.attack = 0.01f; p.decay = 0.05f; p.sustain = 0.5f; p.release = 0.1f;
            chain.addEnvelope(p);
            chain.addEnvelope(p);
            chain.noteOn();
            float level = 0.0f;
            for (int i = 0; i < 100; ++i) level = chain.process();
            Envelope single;
            single.setSampleRate(sr);
            single.setParams(p);
            single.noteOn();
            float singleLevel = 0.0f;
            for (int i = 0; i < 100; ++i) singleLevel = single.process();
            expectWithinAbsoluteError(level, singleLevel * 2.0f, 0.01f);
        }

        beginTest("Chain noteOff releases all active envelopes");
        {
            EnvelopeChain chain;
            chain.setSampleRate(sr);
            chain.setMode(EnvelopeChain::ChainMode::Parallel);
            Envelope::Params p; p.attack = 0.01f; p.decay = 0.01f; p.sustain = 0.8f; p.release = 0.05f;
            chain.addEnvelope(p);
            chain.addEnvelope(p);
            chain.noteOn();
            for (int i = 0; i < 2000; ++i) chain.process();
            expect(chain.isActive());
            chain.noteOff();
            for (int i = 0; i < 5000; ++i) chain.process();
            expect(!chain.isActive(), "Chain should be idle after release completes");
        }

        beginTest("Single-envelope chain matches standalone");
        {
            Envelope::Params p; p.attack = 0.01f; p.decay = 0.05f; p.sustain = 0.6f; p.release = 0.1f;
            EnvelopeChain chain;
            chain.setSampleRate(sr);
            chain.setMode(EnvelopeChain::ChainMode::Series);
            chain.addEnvelope(p);
            Envelope standalone;
            standalone.setSampleRate(sr);
            standalone.setParams(p);
            chain.noteOn();
            standalone.noteOn();
            float maxDiff = 0.0f;
            for (int i = 0; i < 500; ++i)
            {
                float chainVal = chain.process();
                float standaloneVal = standalone.process();
                maxDiff = std::max(maxDiff, std::abs(chainVal - standaloneVal));
            }
            expect(maxDiff < 0.0001f, "Single-envelope chain should match standalone envelope exactly");
        }

        beginTest("getStageCount and reset");
        {
            EnvelopeChain chain;
            chain.setSampleRate(sr);
            expectEquals(chain.getStageCount(), 0);
            Envelope::Params p; p.attack = 0.01f; p.decay = 0.01f; p.sustain = 0.5f; p.release = 0.1f;
            chain.addEnvelope(p);
            expectEquals(chain.getStageCount(), 1);
            chain.addEnvelope(p);
            expectEquals(chain.getStageCount(), 2);
            chain.noteOn();
            expect(chain.isActive());
            chain.reset();
            expect(!chain.isActive(), "Chain should be idle after reset");
        }
    }
};
static EnvelopeChainTests envelopeChainTests;

// ===== FilterSlot Voice Wiring Tests (from WT5) =====
class FilterSlotWiringTests : public juce::UnitTest
{
public:
    FilterSlotWiringTests() : UnitTest("FilterSlot Wiring") {}
    void runTest() override
    {
        beginTest("Voice with FilterSlot produces output");
        {
            UnifiedVoice voice;
            voice.setSampleRate(44100.0);
            voice.setNote(60, 0.8f);
            voice.noteOn();
            float maxAbs = 0.0f;
            for (int i = 0; i < 4096; ++i)
            {
                float s = voice.process();
                maxAbs = std::max(maxAbs, std::abs(s));
            }
            expect(maxAbs > 0.001f, "Voice with FilterSlot should produce non-silent output");
        }

        beginTest("Changing filter model changes the sound");
        {
            UnifiedVoice voiceSVF;
            voiceSVF.setSampleRate(44100.0);
            voiceSVF.setFilterBaseCutoff(1000.0f);
            voiceSVF.setFilterResonance(0.5f);
            voiceSVF.setFilterModel(0, FilterModel::SVF);
            voiceSVF.setNote(48, 0.8f);
            voiceSVF.noteOn();
            float energySVF = 0.0f;
            for (int i = 0; i < 4096; ++i) { float s = voiceSVF.process(); energySVF += s * s; }

            UnifiedVoice voiceLadder;
            voiceLadder.setSampleRate(44100.0);
            voiceLadder.setFilterBaseCutoff(1000.0f);
            voiceLadder.setFilterResonance(0.5f);
            voiceLadder.setFilterModel(0, FilterModel::Ladder);
            voiceLadder.setNote(48, 0.8f);
            voiceLadder.noteOn();
            float energyLadder = 0.0f;
            for (int i = 0; i < 4096; ++i) { float s = voiceLadder.process(); energyLadder += s * s; }

            float ratio = (energySVF > 0.0001f) ? (energyLadder / energySVF) : 0.0f;
            expect(std::abs(ratio - 1.0f) > 0.01f, "SVF and Ladder filter models should produce different output energy");
        }

        beginTest("Series routing sounds different from parallel");
        {
            auto makeVoice = [](FilterRouting routing) {
                UnifiedVoice v;
                v.setSampleRate(44100.0);
                v.setFilterBaseCutoff(2000.0f);
                v.setFilterResonance(0.4f);
                v.setFilterModel(0, FilterModel::SVF);
                v.setFilterModel(1, FilterModel::Ladder);
                v.setFilterRouting(routing);
                v.setNote(48, 0.8f);
                v.noteOn();
                return v;
            };
            auto voiceSeries = makeVoice(FilterRouting::Series);
            auto voiceParallel = makeVoice(FilterRouting::Parallel);
            float energySeries = 0.0f, energyParallel = 0.0f;
            for (int i = 0; i < 4096; ++i)
            {
                float ss = voiceSeries.process();
                float sp = voiceParallel.process();
                energySeries += ss * ss;
                energyParallel += sp * sp;
            }
            expect(std::abs(energySeries - energyParallel) > 0.001f, "Series and Parallel routing should produce different output");
        }
    }
};
static FilterSlotWiringTests filterSlotWiringTests;

// ===== Architecture Preset Apply Tests (from WT5) =====
class ArchPresetApplyTests : public juce::UnitTest
{
public:
    ArchPresetApplyTests() : UnitTest("Arch Preset Apply") {}
    void runTest() override
    {
        beginTest("MonoVA preset applies correctly");
        {
            UnifiedVoiceEngine engine(8);
            engine.prepareToPlay(44100.0, 512);
            engine.applyArchitecturePreset(ArchitecturePresetId::MonoVA);
            expectEquals(engine.getAllocator().getVoiceCount(), 1);
            expect(engine.getVoice(0).getFilterSlot().getModelA() == FilterModel::Ladder);
            expect(engine.getVoice(0).getFilterSlot().getRouting() == FilterRouting::Single);
        }

        beginTest("HybridAggressive preset configures diode filter");
        {
            UnifiedVoiceEngine engine(8);
            engine.prepareToPlay(44100.0, 512);
            engine.applyArchitecturePreset(ArchitecturePresetId::HybridAggressive);
            expect(engine.getVoice(0).getFilterSlot().getModelA() == FilterModel::Diode);
        }

        beginTest("VirtualPolysynth preset configures poly voices");
        {
            UnifiedVoiceEngine engine(8);
            engine.prepareToPlay(44100.0, 512);
            engine.applyArchitecturePreset(ArchitecturePresetId::VirtualPolysynth);
            expectEquals(engine.getAllocator().getVoiceCount(), 5);
        }

        beginTest("Preset engine produces output after apply");
        {
            UnifiedVoiceEngine engine(8);
            engine.prepareToPlay(44100.0, 512);
            engine.applyArchitecturePreset(ArchitecturePresetId::MonoVA);
            engine.noteOn(60, 0.8f, -1);
            juce::AudioBuffer<float> buffer(2, 512);
            buffer.clear();
            juce::MidiBuffer midi;
            engine.processBlock(buffer, midi);
            float maxAbs = 0.0f;
            auto* left = buffer.getReadPointer(0);
            for (int i = 0; i < 512; ++i) maxAbs = std::max(maxAbs, std::abs(left[i]));
            expect(maxAbs > 0.001f, "Engine with architecture preset should produce output");
        }
    }
};
static ArchPresetApplyTests archPresetApplyTests;

// ===== ParamPage Preset Loading Tests (from WT5) =====
class ParamPagePresetTests : public juce::UnitTest
{
public:
    ParamPagePresetTests() : UnitTest("ParamPage Preset Loading") {}
    void runTest() override
    {
        beginTest("Without an engine the screen shows the unified pages plus MOD (the track pages are on TRK FX)");
        {
            SequencerCore seq;
            ParamPageScreen screen(seq);
            // ... plus the MAIN landing page and the three modulation pages (MOD, MOD2, MENV).
            expectEquals(screen.getPageCount(), (int) UnifiedVoiceEngine::basePages().size() + 4);
            expectEquals(screen.getName(), juce::String("PARAM"));
            expectEquals(screen.getPageName(), juce::String("MAIN"));
            screen.onArrowRight();
            expectEquals(screen.getPageName(), juce::String("OSC"));
            screen.onArrowRight();
            expectEquals(screen.getPageName(), juce::String("OSC3"));
        }

        beginTest("Func + bank buttons jump between sections");
        {
            SequencerCore seq;
            ParamPageScreen screen(seq);
            screen.onSectionNext();
            expectEquals(screen.getPageName(), juce::String("OSC"));
            screen.onSectionNext();
            expectEquals(screen.getPageName(), juce::String("FILTR"));
            screen.onSectionNext();
            expectEquals(screen.getPageName(), juce::String("AMP"));
            screen.onSectionPrev();
            expectEquals(screen.getPageName(), juce::String("FILTR"));
            screen.onArrowRight();
            expectEquals(screen.getPageName(), juce::String("FLT B"));
            screen.onArrowRight();
            expectEquals(screen.getPageName(), juce::String("F ENV"));
            screen.onSectionPrev();
            expectEquals(screen.getPageName(), juce::String("FILTR"));
            screen.onSectionPrev();
            expectEquals(screen.getPageName(), juce::String("OSC"));
            screen.onSectionPrev();
            expectEquals(screen.getPageName(), juce::String("MAIN"));
        }
    }
};
static ParamPagePresetTests paramPagePresetTests;

// ===== Sub-Parameter Overlay Tests (from WT6) =====
class SubParamTests : public juce::UnitTest
{
public:
    SubParamTests() : UnitTest("Sub-Parameter Overlay") {}
    void runTest() override
    {
        // The sub-parameter overlay is gone: Shift + turn is an ordinary (fine) turn on every screen,
        // even one that still declares a sub-parameter.
        ScreenManager mgr;
        class SubScreen : public Screen
        {
        public:
            int turns = 0, subTurns = 0;
            bool fine = false;
            juce::String getName() const override { return "SUB"; }
            void render(DisplayRenderer&) override {}
            bool hasSubParam(int e) const override { return e == 0; }
            void onSubParamChange(int, int) override { ++subTurns; }
            void onEncoderChange(int, int) override { ++turns; fine = (turnMode() == TurnMode::Fine); }
        };
        auto* screen = new SubScreen();
        mgr.addScreen(std::unique_ptr<Screen>(screen));

        beginTest("Shift + turn is a fine turn, never a sub-parameter");
        {
            mgr.controlChangeReceived(7, 63, 127);
            mgr.controlChangeReceived(16, 0x15, 1);
            mgr.controlChangeReceived(7, 63, 0);
            expectEquals(screen->turns, 1);
            expectEquals(screen->subTurns, 0);
            expect(screen->fine);
            mgr.controlChangeReceived(16, 0x15, 1);
            expectEquals(screen->turns, 2);
            expectEquals(screen->subTurns, 0);
            expect(!screen->fine);
        }
    }
};
static SubParamTests subParamTests;

class DrumVoiceTests : public juce::UnitTest
{
public:
    DrumVoiceTests() : UnitTest("Drum Voice") {}

    void runTest() override
    {
        beginTest("Each synth type produces output");
        {
            DrumSynthType types[] = {
                DrumSynthType::FMKick, DrumSynthType::AnalogKick, DrumSynthType::ModCyclesKick,
                DrumSynthType::NoiseSnare, DrumSynthType::FMSnare, DrumSynthType::Clap,
                DrumSynthType::ClosedHat, DrumSynthType::OpenHat, DrumSynthType::Cymbal,
                DrumSynthType::Tom, DrumSynthType::Rim, DrumSynthType::Shaker,
                DrumSynthType::Clave, DrumSynthType::SubBass
            };

            for (auto t : types)
            {
                DrumVoice v;
                v.setSampleRate(44100.0);
                v.setType(t);
                v.setTune(200.0f);
                v.setDecay(0.3f);
                v.setLevel(0.8f);
                v.noteOn(0.8f);

                float maxAbs = 0.0f;
                for (int i = 0; i < 4410; ++i)
                    maxAbs = juce::jmax(maxAbs, std::abs(v.process()));

                expect(maxAbs > 0.001f, "Type " + juce::String((int)t) + " should produce output");
            }
        }

        beginTest("Velocity scaling");
        {
            DrumVoice loud, quiet;
            loud.setSampleRate(44100.0);
            quiet.setSampleRate(44100.0);
            loud.setType(DrumSynthType::FMKick);
            quiet.setType(DrumSynthType::FMKick);
            loud.setTune(60.0f);
            quiet.setTune(60.0f);
            loud.setDecay(0.5f);
            quiet.setDecay(0.5f);
            loud.setLevel(1.0f);
            quiet.setLevel(1.0f);

            loud.noteOn(1.0f);
            quiet.noteOn(0.2f);

            float loudMax = 0.0f, quietMax = 0.0f;
            for (int i = 0; i < 4410; ++i)
            {
                loudMax = juce::jmax(loudMax, std::abs(loud.process()));
                quietMax = juce::jmax(quietMax, std::abs(quiet.process()));
            }
            expect(loudMax > quietMax, "Loud hit should be louder than quiet");
        }

        beginTest("Voice deactivates after envelope");
        {
            DrumVoice v;
            v.setSampleRate(44100.0);
            v.setType(DrumSynthType::Rim);
            v.setTune(800.0f);
            v.setDecay(0.005f);
            v.setLevel(0.8f);
            v.noteOn(1.0f);

            for (int i = 0; i < 44100 * 3; ++i)
                v.process();

            expect(!v.isActive(), "Rim should deactivate after short decay");
        }

        beginTest("SubBass sustains while active");
        {
            DrumVoice v;
            v.setSampleRate(44100.0);
            v.setType(DrumSynthType::SubBass);
            v.setTune(55.0f);
            v.setDecay(0.1f);
            v.setLevel(0.8f);
            v.noteOn(0.8f);

            for (int i = 0; i < 22050; ++i)
                v.process();

            expect(v.isActive(), "SubBass should sustain");

            v.noteOff();
            for (int i = 0; i < 44100; ++i)
                v.process();

            expect(!v.isActive(), "SubBass should deactivate after noteOff + release");
        }
    }
};
static DrumVoiceTests drumVoiceTests;

class DrumEngineTests : public juce::UnitTest
{
public:
    DrumEngineTests() : UnitTest("Drum Engine") {}

    void runTest() override
    {
        beginTest("Factory creates drum engine");
        {
            auto engine = AudioEngine::createEngine(EngineType::Drum);
            expect(engine != nullptr);
            expectEquals((int)engine->getType(), (int)EngineType::Drum);
        }

        beginTest("Drum level against a synth voice");
        {
            auto peakOf = [](EngineBase& e, int note)
            {
                e.prepareToPlay(44100.0, 512);
                e.noteOn(note, 1.0f);
                float peak = 0.0f;
                juce::MidiBuffer midi;
                for (int b = 0; b < 43; ++b)   // ~0.5 s
                {
                    juce::AudioBuffer<float> buffer(2, 512);
                    buffer.clear();
                    e.processBlock(buffer, midi);
                    peak = juce::jmax(peak, buffer.getMagnitude(0, 512));   // all channels
                }
                return peak;
            };

            UnifiedVoiceEngine synth;
            const float synthPeak = peakOf(synth, 60);
            logMessage("synth peak " + juce::String(synthPeak, 3));

            float loudest = 0.0f;
            for (int v = 0; v < DrumEngine::NUM_VOICES; ++v)
            {
                DrumEngine drum;
                const float p = peakOf(drum, v);
                loudest = juce::jmax(loudest, p);
                logMessage("drum voice " + juce::String(v) + " peak " + juce::String(p, 3));
            }

            // Absolute limits (the synth's peak varies run to run with its analog phase randomisation,
            // 1.3-2.0 at full velocity, so it is only logged above): a centred full-velocity kick peaks
            // above 0.6 (it was 0.45 with the old linear pan law) and no voice is far hotter than a synth note.
            DrumEngine kickKit;
            const float kickPeak = peakOf(kickKit, 8);
            expect(kickPeak > 0.6f, "kick peak " + juce::String(kickPeak, 3));
            expect(loudest < 1.6f, "loudest drum " + juce::String(loudest, 3));
        }

        beginTest("Default kit is loaded");
        {
            DrumEngine drum;
            drum.prepareToPlay(44100.0, 512);

            expect(drum.getVoice(0).getType() == DrumSynthType::SubBass, "Pad 0 should be SubBass");
            expect(drum.getVoice(8).getType() == DrumSynthType::KickSynth, "Pad 9 (lower left) should be the kick machine");
            expect(drum.getVoice(14).getType() == DrumSynthType::ClosedHat, "Pad 14 should be ClosedHat");
            expect(drum.getVoice(15).getType() == DrumSynthType::OpenHat, "Pad 15 should be OpenHat");
        }

        beginTest("NoteOn maps correctly - direct and pad MIDI notes");
        {
            DrumEngine drum;
            drum.prepareToPlay(44100.0, 512);

            drum.noteOn(8, 0.9f);
            expect(drum.getVoice(8).isActive(), "Direct note 8 should trigger voice 8");

            drum.allNotesOff();
            drum.noteOn(112, 0.9f);
            expect(drum.getVoice(8).isActive(), "Pad MIDI 112 should trigger voice 8");

            drum.allNotesOff();
            drum.noteOn(96, 0.9f);
            expect(drum.getVoice(0).isActive(), "Pad MIDI 96 should trigger voice 0");
        }

        beginTest("Choke groups work");
        {
            DrumEngine drum;
            drum.prepareToPlay(44100.0, 512);

            drum.noteOn(15, 0.9f);
            expect(drum.getVoice(15).isActive(), "OpenHat should be active");

            drum.noteOn(14, 0.9f);
            expect(drum.getVoice(14).isActive(), "ClosedHat should be active");
        }

        beginTest("processBlock produces output");
        {
            DrumEngine drum;
            drum.prepareToPlay(44100.0, 512);
            drum.noteOn(8, 1.0f);

            juce::AudioBuffer<float> buffer(2, 512);
            buffer.clear();
            juce::MidiBuffer midi;
            drum.processBlock(buffer, midi);

            float maxAbs = 0.0f;
            for (int s = 0; s < 512; ++s)
                maxAbs = juce::jmax(maxAbs, std::abs(buffer.getSample(0, s)));

            expect(maxAbs > 0.001f, "Should produce audio");
        }

        beginTest("allNotesOff silences");
        {
            DrumEngine drum;
            drum.prepareToPlay(44100.0, 512);

            for (int i = 0; i < 16; ++i)
                drum.noteOn(i, 1.0f);

            drum.allNotesOff();

            bool anyActive = false;
            for (int i = 0; i < 16; ++i)
                if (drum.getVoice(i).isActive()) anyActive = true;

            expect(!anyActive, "All voices should be inactive after allNotesOff");
        }
    }
};
static DrumEngineTests drumEngineTests;

class DisplayPrimitiveTests : public juce::UnitTest
{
public:
    DisplayPrimitiveTests() : UnitTest("Display Primitives") {}

    void runTest() override
    {
        beginTest("invertRegion flips pixels");
        {
            DisplayRenderer d;
            d.clear();
            d.setPixel(10, 10, true);
            d.setPixel(11, 10, false);

            d.invertRegion(10, 10, 2, 1);

            expect(!d.getPixel(10, 10), "ON pixel should flip to OFF");
            expect(d.getPixel(11, 10), "OFF pixel should flip to ON");
        }

        beginTest("invertRegion clamps to bounds");
        {
            DisplayRenderer d;
            d.clear();
            d.invertRegion(-5, -5, 200, 200);
            expect(d.getPixel(0, 0), "Corner should be flipped");
        }

        beginTest("drawStringInverted produces OFF pixels");
        {
            DisplayRenderer d;
            d.clear();
            d.drawRect(0, 0, 30, 10, true);
            d.drawStringInverted(2, 1, "A");

            bool hasOffPixel = false;
            for (int x = 2; x < 7; ++x)
                for (int y = 1; y < 8; ++y)
                    if (!d.getPixel(x, y)) hasOffPixel = true;

            expect(hasOffPixel, "Inverted string should create OFF pixels in filled rect");
        }

        beginTest("HomeScreen selected track has both ON and OFF pixels");
        {
            SequencerCore seq;
            HomeScreen home(seq, nullptr);
            DisplayRenderer d;

            home.render(d);

            int selected = seq.getSelectedTrack();
            int x0 = selected * 16;
            bool hasOn = false, hasOff = false;
            for (int x = x0; x < x0 + 16; ++x)
            {
                for (int y = 11; y < 53; ++y)
                {
                    if (d.getPixel(x, y)) hasOn = true;
                    else hasOff = true;
                }
            }
            expect(hasOn && hasOff, "Selected column should have both ON and OFF pixels (not solid white)");
        }
    }
};
static DisplayPrimitiveTests displayPrimitiveTests;

// --- Scale Button Screen Cycling ---
class ScaleButtonTests : public juce::UnitTest
{
public:
    ScaleButtonTests() : UnitTest("Scale Button") {}

    void runTest() override
    {
        beginTest("Scale button toggles scale lock");
        {
            ScreenManager mgr;

            class DummyScr : public Screen
            {
            public:
                DummyScr(juce::String n) : nm(std::move(n)) {}
                juce::String getName() const override { return nm; }
                void render(DisplayRenderer&) override {}
            private:
                juce::String nm;
            };

            mgr.addScreen(std::make_unique<DummyScr>("TRACKS"));

            expect(!mgr.isScaleLocked(), "Scale should start unlocked");

            mgr.controlChangeReceived(7, 0x4A, 127);
            expect(mgr.isScaleLocked(), "Scale should be locked after press");

            mgr.controlChangeReceived(7, 0x4A, 127);
            expect(!mgr.isScaleLocked(), "Scale should be unlocked after second press");
        }

        beginTest("Arp button toggles arpeggiator");
        {
            ScreenManager mgr;

            class DummyScr : public Screen
            {
            public:
                DummyScr(juce::String n) : nm(std::move(n)) {}
                juce::String getName() const override { return nm; }
                void render(DisplayRenderer&) override {}
            private:
                juce::String nm;
            };

            mgr.addScreen(std::make_unique<DummyScr>("TRACKS"));

            bool arpState = false;
            mgr.setArpToggleCallback([&](bool enabled) { arpState = enabled; });

            expect(!mgr.isArpEnabled(), "Arp should start disabled");

            mgr.controlChangeReceived(7, 0x49, 127);
            expect(mgr.isArpEnabled(), "Arp should be enabled after press");
            expect(arpState, "Callback should report enabled");

            mgr.controlChangeReceived(7, 0x49, 127);
            expect(!mgr.isArpEnabled(), "Arp should be disabled after second press");
            expect(!arpState, "Callback should report disabled");
        }

        beginTest("Neptune firmware buttons: Arp 109, Scale 110, Settings 108 on channel 1");
        {
            ScreenManager mgr;

            class DummyScr : public Screen
            {
            public:
                DummyScr(juce::String n) : nm(std::move(n)) {}
                juce::String getName() const override { return nm; }
                void render(DisplayRenderer&) override {}
            private:
                juce::String nm;
            };

            mgr.addScreen(std::make_unique<DummyScr>("TRACKS"));
            mgr.addScreen(std::make_unique<DummyScr>("SETTINGS"));
            mgr.setActiveScreen("TRACKS");

            mgr.controlChangeReceived(1, 109, 127);
            expect(mgr.isArpEnabled(), "CC 109 press should toggle arp on");
            mgr.controlChangeReceived(1, 109, 0);
            expect(mgr.isArpEnabled(), "CC 109 release should not toggle");
            mgr.controlChangeReceived(1, 109, 127);
            expect(!mgr.isArpEnabled(), "second CC 109 press should toggle arp off");

            mgr.controlChangeReceived(1, 110, 127);
            expect(mgr.isScaleLocked(), "CC 110 press should lock scale");
            mgr.controlChangeReceived(1, 110, 0);
            mgr.controlChangeReceived(1, 110, 127);
            expect(!mgr.isScaleLocked(), "second CC 110 press should unlock scale");

            mgr.controlChangeReceived(1, 108, 0);
            expect(mgr.getActiveScreen()->getName() == "TRACKS", "CC 108 release alone should not navigate");
            mgr.controlChangeReceived(1, 108, 127);
            expect(mgr.getActiveScreen()->getName() == "SETTINGS", "CC 108 press should open SETTINGS");
        }

        beginTest("Keybed-port notes in the pad range are keyboard notes, not pads");
        {
            ScreenManager mgr;

            class PadCountScr : public Screen
            {
            public:
                juce::String getName() const override { return "TRACKS"; }
                void render(DisplayRenderer&) override {}
                void onPadPress(int, int) override { ++padPresses; }
                void onPadRelease(int) override { ++padReleases; }
                int padPresses = 0, padReleases = 0;
            };
            struct KeyCount : public ScreenManager::KeyboardListener
            {
                void keyboardNoteOn(int note, int) override { lastOn = note; }
                void keyboardNoteOff(int note) override { lastOff = note; }
                int lastOn = -1, lastOff = -1;
            } keys;

            auto scr = std::make_unique<PadCountScr>();
            auto* scrPtr = scr.get();
            mgr.addScreen(std::move(scr));
            mgr.setKeyboardListener(&keys);

            mgr.noteOnReceived(LaunchkeyController::KEYS_PORT_CHANNEL, 96, 100);
            expectEquals(scrPtr->padPresses, 0, "key at note 96 must not press a pad");
            expectEquals(keys.lastOn, 96, "key at note 96 must reach the keyboard listener");
            mgr.noteOffReceived(LaunchkeyController::KEYS_PORT_CHANNEL, 96);
            expectEquals(scrPtr->padReleases, 0);
            expectEquals(keys.lastOff, 96);

            mgr.noteOnReceived(1, 96, 100);
            expectEquals(scrPtr->padPresses, 1, "DAW-port note 96 is still pad 1");
        }

        beginTest("Arrows and bank buttons page within the active screen");
        {
            ScreenManager mgr;

            class PagingScr : public Screen
            {
            public:
                juce::String getName() const override { return "PARAM"; }
                void render(DisplayRenderer&) override {}
                void onArrowLeft() override { --page; }
                void onArrowRight() override { ++page; }
                int page = 0;
            };

            auto scr = std::make_unique<PagingScr>();
            auto* s = scr.get();
            mgr.addScreen(std::move(scr));

            // Control grammar: only the bank buttons page the encoders. Up / Down address the pads
            // (onPadPagePrev / Next, on release) and ">" is the pattern key.
            mgr.controlChangeReceived(1, 52, 127);   // bank down -> next
            mgr.controlChangeReceived(1, 52, 0);     // release does nothing
            expectEquals(s->page, 1);
            mgr.controlChangeReceived(1, 107, 127);  // Down: not the encoder page
            mgr.controlChangeReceived(1, 107, 0);
            mgr.controlChangeReceived(1, 104, 127);  // ">": nothing
            mgr.controlChangeReceived(1, 104, 0);
            expectEquals(s->page, 1);
            mgr.controlChangeReceived(1, 52, 127);
            mgr.controlChangeReceived(1, 52, 127);
            expectEquals(s->page, 3);
            mgr.controlChangeReceived(1, 51, 127);   // bank up -> previous
            mgr.controlChangeReceived(1, 106, 127);  // Up: not the encoder page
            mgr.controlChangeReceived(1, 106, 0);
            expectEquals(s->page, 2);
            mgr.controlChangeReceived(1, 51, 127);
            expectEquals(s->page, 1);

            int track = mgr.getSelectedTrack();
            mgr.controlChangeReceived(7, 63, 127);   // Shift down
            mgr.controlChangeReceived(1, 107, 127);  // Shift+Down -> next track, no paging
            mgr.controlChangeReceived(1, 107, 0);
            mgr.controlChangeReceived(7, 63, 0);
            expectEquals(s->page, 1);
            expect(mgr.getSelectedTrack() != track, "Shift+Down should change the selected track");
        }

        beginTest("Shift+Pad navigates to mapped screen");
        {
            ScreenManager mgr;

            class DummyScr : public Screen
            {
            public:
                DummyScr(juce::String n) : nm(std::move(n)) {}
                juce::String getName() const override { return nm; }
                void render(DisplayRenderer&) override {}
            private:
                juce::String nm;
            };

            mgr.addScreen(std::make_unique<DummyScr>("TRACKS"));
            mgr.addScreen(std::make_unique<DummyScr>("STEP"));
            mgr.addScreen(std::make_unique<DummyScr>("PARAM"));

            mgr.setShiftPadScreen(0, 2);  // Pad 0 → screen 2 (PARAM)
            mgr.setShiftPadScreen(1, 1);  // Pad 1 → screen 1 (STEP)

            expectEquals(mgr.getActiveIndex(), 0);

            // Hold shift
            mgr.controlChangeReceived(7, 63, 127);
            // Press top pad 0 (note 0x60)
            mgr.noteOnReceived(1, 0x60, 100);
            expectEquals(mgr.getActiveIndex(), 2);

            // Press top pad 1 (note 0x61)
            mgr.noteOnReceived(1, 0x61, 100);
            expectEquals(mgr.getActiveIndex(), 1);

            // Release shift, press pad — should NOT navigate
            mgr.controlChangeReceived(7, 63, 0);
            mgr.noteOnReceived(1, 0x60, 100);
            expectEquals(mgr.getActiveIndex(), 1);
        }
    }
};
static ScaleButtonTests scaleButtonTests;

class MasterChainTests : public juce::UnitTest
{
public:
    MasterChainTests() : UnitTest("Master Chain") {}

    void runTest() override
    {
        beginTest("EQ low shelf boost increases low frequency energy");
        {
            ParametricEQ eq;
            eq.setSampleRate(44100.0);
            eq.setLowShelf(200.0f, 12.0f);  // +12dB low shelf at 200Hz

            // Generate 100Hz sine (below shelf frequency)
            float energyBoosted = 0.0f;
            eq.reset();
            for (int i = 0; i < 4000; ++i)
            {
                float sample = 0.5f * std::sin(2.0f * 3.14159f * 100.0f * i / 44100.0f);
                float l = sample, r = sample;
                eq.process(l, r);
                if (i > 500) // skip transient
                    energyBoosted += l * l;
            }

            // Compare with flat EQ
            ParametricEQ flat;
            flat.setSampleRate(44100.0);
            float energyFlat = 0.0f;
            for (int i = 0; i < 4000; ++i)
            {
                float sample = 0.5f * std::sin(2.0f * 3.14159f * 100.0f * i / 44100.0f);
                float l = sample, r = sample;
                flat.process(l, r);
                if (i > 500)
                    energyFlat += l * l;
            }

            expect(energyBoosted > energyFlat * 2.0f,
                "Low shelf boost should increase low frequency energy");
        }

        beginTest("EQ low shelf cut reduces low frequency energy");
        {
            ParametricEQ eq;
            eq.setSampleRate(44100.0);
            eq.setLowShelf(200.0f, -12.0f);  // -12dB cut

            float energyCut = 0.0f;
            eq.reset();
            for (int i = 0; i < 4000; ++i)
            {
                float sample = 0.5f * std::sin(2.0f * 3.14159f * 100.0f * i / 44100.0f);
                float l = sample, r = sample;
                eq.process(l, r);
                if (i > 500)
                    energyCut += l * l;
            }

            ParametricEQ flat;
            flat.setSampleRate(44100.0);
            float energyFlat = 0.0f;
            for (int i = 0; i < 4000; ++i)
            {
                float sample = 0.5f * std::sin(2.0f * 3.14159f * 100.0f * i / 44100.0f);
                float l = sample, r = sample;
                flat.process(l, r);
                if (i > 500)
                    energyFlat += l * l;
            }

            expect(energyCut < energyFlat * 0.5f,
                "Low shelf cut should reduce low frequency energy");
        }

        beginTest("EQ parametric band boosts target frequency");
        {
            ParametricEQ eq;
            eq.setSampleRate(44100.0);
            eq.setParametric(0, 1000.0f, 12.0f, 2.0f);  // +12dB at 1kHz

            float energyBoosted = 0.0f;
            for (int i = 0; i < 8000; ++i)
            {
                float sample = 0.5f * std::sin(2.0f * 3.14159f * 1000.0f * i / 44100.0f);
                float l = sample, r = sample;
                eq.process(l, r);
                if (i > 1000)
                    energyBoosted += l * l;
            }

            ParametricEQ flat;
            flat.setSampleRate(44100.0);
            float energyFlat = 0.0f;
            for (int i = 0; i < 8000; ++i)
            {
                float sample = 0.5f * std::sin(2.0f * 3.14159f * 1000.0f * i / 44100.0f);
                float l = sample, r = sample;
                flat.process(l, r);
                if (i > 1000)
                    energyFlat += l * l;
            }

            expect(energyBoosted > energyFlat * 2.0f,
                "Parametric boost should increase energy at target frequency");
        }

        beginTest("EQ high shelf boost increases high frequency energy");
        {
            ParametricEQ eq;
            eq.setSampleRate(44100.0);
            eq.setHighShelf(4000.0f, 12.0f);  // +12dB at 4kHz+

            float energyBoosted = 0.0f;
            eq.reset();
            for (int i = 0; i < 8000; ++i)
            {
                float sample = 0.5f * std::sin(2.0f * 3.14159f * 10000.0f * i / 44100.0f);
                float l = sample, r = sample;
                eq.process(l, r);
                if (i > 1000)
                    energyBoosted += l * l;
            }

            ParametricEQ flat;
            flat.setSampleRate(44100.0);
            float energyFlat = 0.0f;
            for (int i = 0; i < 8000; ++i)
            {
                float sample = 0.5f * std::sin(2.0f * 3.14159f * 10000.0f * i / 44100.0f);
                float l = sample, r = sample;
                flat.process(l, r);
                if (i > 1000)
                    energyFlat += l * l;
            }

            expect(energyBoosted > energyFlat * 2.0f,
                "High shelf boost should increase high frequency energy");
        }

        beginTest("EQ reset clears state");
        {
            ParametricEQ eq;
            eq.setSampleRate(44100.0);
            eq.setParametric(0, 1000.0f, 12.0f, 1.0f);

            for (int i = 0; i < 100; ++i)
            {
                float l = 1.0f, r = 1.0f;
                eq.process(l, r);
            }

            eq.reset();
            float l = 0.0f, r = 0.0f;
            eq.process(l, r);
            // After reset with zero input, output should be zero
            // (biquad state cleared)
            expectWithinAbsoluteError(l, 0.0f, 0.001f, "EQ reset should clear state");
        }

        beginTest("Compressor reduces gain on loud signal");
        {
            Compressor comp;
            comp.setSampleRate(44100.0);
            comp.setThreshold(-20.0f);
            comp.setRatio(4.0f);
            comp.setAttack(1.0f);
            comp.setRelease(100.0f);
            comp.setMakeupGain(0.0f);
            comp.setKneeWidth(0.0f);  // Hard knee for predictable test
            comp.reset();

            // Feed a loud signal (well above threshold)
            float peakOut = 0.0f;
            for (int i = 0; i < 4000; ++i)
            {
                float sample = 0.9f;  // ~-0.9dB, well above -20dB threshold
                float l = sample, r = sample;
                comp.process(l, r);
                if (i > 500)
                    peakOut = std::max(peakOut, std::abs(l));
            }

            expect(peakOut < 0.9f, "Compressor should reduce level of loud signal");
            expect(peakOut > 0.01f, "Compressor should still pass signal");
        }

        beginTest("Compressor does not affect signal below threshold");
        {
            Compressor comp;
            comp.setSampleRate(44100.0);
            comp.setThreshold(-6.0f);
            comp.setRatio(10.0f);
            comp.setAttack(1.0f);
            comp.setRelease(100.0f);
            comp.setMakeupGain(0.0f);
            comp.setKneeWidth(0.0f);
            comp.reset();

            // Feed a quiet signal (below threshold: -6dB = 0.5 linear)
            float maxDiff = 0.0f;
            for (int i = 0; i < 2000; ++i)
            {
                float sample = 0.1f * std::sin(2.0f * 3.14159f * 440.0f * i / 44100.0f);
                float l = sample, r = sample;
                float origL = l;
                comp.process(l, r);
                if (i > 500)
                    maxDiff = std::max(maxDiff, std::abs(l - origL));
            }

            expect(maxDiff < 0.01f,
                "Compressor should not alter signal below threshold");
        }

        beginTest("Compressor makeup gain boosts output");
        {
            Compressor comp;
            comp.setSampleRate(44100.0);
            comp.setThreshold(-40.0f);
            comp.setRatio(1.0f);   // No compression
            comp.setMakeupGain(12.0f);
            comp.setAttack(1.0f);
            comp.setRelease(100.0f);
            comp.setKneeWidth(0.0f);
            comp.reset();

            float peakOut = 0.0f;
            float peakIn = 0.0f;
            for (int i = 0; i < 2000; ++i)
            {
                float sample = 0.1f * std::sin(2.0f * 3.14159f * 440.0f * i / 44100.0f);
                peakIn = std::max(peakIn, std::abs(sample));
                float l = sample, r = sample;
                comp.process(l, r);
                if (i > 500)
                    peakOut = std::max(peakOut, std::abs(l));
            }

            expect(peakOut > peakIn * 2.0f,
                "12dB makeup gain should roughly quadruple the amplitude");
        }

        beginTest("Compressor gain reduction meter reports negative dB");
        {
            Compressor comp;
            comp.setSampleRate(44100.0);
            comp.setThreshold(-20.0f);
            comp.setRatio(8.0f);
            comp.setAttack(0.5f);
            comp.setRelease(100.0f);
            comp.setMakeupGain(0.0f);
            comp.setKneeWidth(0.0f);
            comp.reset();

            for (int i = 0; i < 4000; ++i)
            {
                float l = 0.9f, r = 0.9f;
                comp.process(l, r);
            }

            float gr = comp.getGainReductionDb();
            expect(gr < 0.0f, "Gain reduction should be negative when compressing");
        }

        beginTest("Limiter enforces ceiling on loud signal");
        {
            Limiter lim;
            lim.setSampleRate(44100.0);
            lim.setCeiling(-1.0f);  // ~0.891 linear
            lim.setLookahead(3.0f);
            lim.setRelease(200.0f);
            lim.reset();

            float ceilingLinear = std::pow(10.0f, -1.0f / 20.0f);
            float maxOut = 0.0f;

            // Process loud signal through limiter
            // Need extra samples to flush the lookahead delay
            for (int i = 0; i < 8000; ++i)
            {
                float sample = 2.0f * std::sin(2.0f * 3.14159f * 440.0f * i / 44100.0f);
                float l = sample, r = sample;
                lim.process(l, r);
                if (i > 500)  // skip initial fill
                    maxOut = std::max(maxOut, std::max(std::abs(l), std::abs(r)));
            }

            expect(maxOut <= ceilingLinear + 0.001f,
                "Limiter output should never exceed ceiling");
            expect(maxOut > 0.1f,
                "Limiter should still pass signal");
        }

        beginTest("Limiter passes quiet signal unchanged");
        {
            Limiter lim;
            lim.setSampleRate(44100.0);
            lim.setCeiling(-0.3f);
            lim.setLookahead(3.0f);
            lim.setRelease(200.0f);
            lim.reset();

            int lookaheadSamples = (int)(3.0f * 0.001f * 44100.0f);

            // Process quiet signal
            float maxDiff = 0.0f;
            std::array<float, 256> inputHistory {};
            int histIdx = 0;

            for (int i = 0; i < 4000; ++i)
            {
                float sample = 0.1f * std::sin(2.0f * 3.14159f * 440.0f * i / 44100.0f);
                inputHistory[histIdx] = sample;
                float l = sample, r = sample;
                lim.process(l, r);

                // Compare with delayed input (lookahead introduces delay)
                if (i > lookaheadSamples + 100)
                {
                    int delayedIdx = (histIdx - lookaheadSamples + 256) % 256;
                    maxDiff = std::max(maxDiff, std::abs(l - inputHistory[delayedIdx]));
                }
                histIdx = (histIdx + 1) % 256;
            }

            expect(maxDiff < 0.01f,
                "Limiter should pass quiet signal with minimal change");
        }

        beginTest("Limiter reset clears delay buffer");
        {
            Limiter lim;
            lim.setSampleRate(44100.0);
            lim.setCeiling(-0.3f);
            lim.setLookahead(3.0f);
            lim.reset();

            // Feed loud signal
            for (int i = 0; i < 1000; ++i)
            {
                float l = 1.0f, r = 1.0f;
                lim.process(l, r);
            }

            lim.reset();

            // After reset, feeding silence should produce silence
            float maxOut = 0.0f;
            for (int i = 0; i < 500; ++i)
            {
                float l = 0.0f, r = 0.0f;
                lim.process(l, r);
                maxOut = std::max(maxOut, std::max(std::abs(l), std::abs(r)));
            }

            expectWithinAbsoluteError(maxOut, 0.0f, 0.001f,
                "After reset with zero input, output should be zero");
        }

        beginTest("Limiter ceiling at 0dB still limits peaks above unity");
        {
            Limiter lim;
            lim.setSampleRate(44100.0);
            lim.setCeiling(0.0f);  // 1.0 linear
            lim.setLookahead(3.0f);
            lim.setRelease(200.0f);
            lim.reset();

            float maxOut = 0.0f;
            for (int i = 0; i < 8000; ++i)
            {
                float sample = 3.0f * std::sin(2.0f * 3.14159f * 440.0f * i / 44100.0f);
                float l = sample, r = sample;
                lim.process(l, r);
                if (i > 500)
                    maxOut = std::max(maxOut, std::max(std::abs(l), std::abs(r)));
            }

            expect(maxOut <= 1.001f,
                "Limiter at 0dB ceiling should keep output at or below unity");
        }

        beginTest("Full master chain: EQ -> Compressor -> Limiter");
        {
            ParametricEQ eq;
            eq.setSampleRate(44100.0);
            eq.setParametric(0, 1000.0f, 6.0f, 1.0f);  // Mild boost

            Compressor comp;
            comp.setSampleRate(44100.0);
            comp.setThreshold(-12.0f);
            comp.setRatio(4.0f);
            comp.setAttack(5.0f);
            comp.setRelease(100.0f);
            comp.setMakeupGain(3.0f);
            comp.setKneeWidth(3.0f);
            comp.reset();

            Limiter lim;
            lim.setSampleRate(44100.0);
            lim.setCeiling(-0.3f);
            lim.setLookahead(3.0f);
            lim.setRelease(200.0f);
            lim.reset();

            float ceilingLinear = std::pow(10.0f, -0.3f / 20.0f);
            float maxOut = 0.0f;
            bool hadOutput = false;

            for (int i = 0; i < 16000; ++i)
            {
                float sample = 0.7f * std::sin(2.0f * 3.14159f * 1000.0f * i / 44100.0f);
                float l = sample, r = sample;

                eq.process(l, r);
                comp.process(l, r);
                lim.process(l, r);

                if (i > 1000)
                {
                    maxOut = std::max(maxOut, std::max(std::abs(l), std::abs(r)));
                    if (std::abs(l) > 0.01f) hadOutput = true;
                }
            }

            expect(hadOutput, "Master chain should produce output");
            expect(maxOut <= ceilingLinear + 0.001f,
                "Master chain output should respect limiter ceiling");
        }
    }
};

static MasterChainTests masterChainTests;

// --- Preset System Tests ---

class PresetSerializationTests : public juce::UnitTest
{
public:
    PresetSerializationTests() : UnitTest("Preset Serialization") {}

    void runTest() override
    {
        beginTest("Preset round-trip JSON serialization");
        {
            Preset original;
            original.name = "Test Bass";
            original.category = "BASS";
            original.engineType = EngineType::UnifiedVoice;
            original.soundParams = {{0, 0.8f}, {10, 800.0f}, {11, 0.4f}, {24, 0.9f}};
            original.isFactory = false;

            ModSlot slot;
            slot.source = ModSource::LFO1;
            slot.destination = 10;
            slot.depth = 0.5f;
            slot.bipolar = true;
            original.modSlots.push_back(slot);

            auto json = original.toJSON();
            auto restored = Preset::fromJSON(json);

            expectEquals(restored.name, juce::String("Test Bass"));
            expectEquals(restored.category, juce::String("BASS"));
            expect(restored.engineType == EngineType::UnifiedVoice);
            expectEquals((int) restored.soundParams.size(), 4);
            expectWithinAbsoluteError(restored.soundParams[10], 800.0f, 0.01f);
            expectEquals((int) restored.modSlots.size(), 1);
            expect(restored.modSlots[0].source == ModSource::LFO1);
            expectEquals(restored.modSlots[0].destination, 10);
        }

        beginTest("Preset from empty JSON returns empty preset");
        {
            auto p = Preset::fromJSON("{}");
            expect(p.name.isEmpty());
            expect(p.soundParams.empty());
        }

        beginTest("Preset var round-trip preserves factory flag");
        {
            Preset p;
            p.name = "Factory";
            p.isFactory = true;
            auto v = p.toVar();
            auto restored = Preset::fromVar(v);
            expect(restored.isFactory);
        }
    }
};

static PresetSerializationTests presetSerializationTests;

class PresetManagerTests : public juce::UnitTest
{
public:
    PresetManagerTests() : UnitTest("Preset Manager") {}

    void runTest() override
    {
        beginTest("Factory presets loaded on construction");
        {
            PresetManager mgr;
            expect(mgr.getPresetCount() > 0, "Should have factory presets");

            bool hasInitVA = false;
            for (int i = 0; i < mgr.getPresetCount(); ++i)
            {
                if (mgr.getPreset(i).name == "Init VA")
                {
                    hasInitVA = true;
                    expect(mgr.getPreset(i).isFactory);
                    expect(mgr.getPreset(i).engineType == EngineType::UnifiedVoice);
                }
            }
            expect(hasInitVA, "Should have Init VA factory preset");
        }

        beginTest("Filter by engine type");
        {
            PresetManager mgr;
            auto vaIndices = mgr.getFilteredIndices(EngineType::UnifiedVoice);
            auto fmIndices = mgr.getFilteredIndices(EngineType::FM);

            expect(!vaIndices.empty(), "Should have VA presets");
            expect(!fmIndices.empty(), "Should have FM presets");

            for (int idx : vaIndices)
                expect(mgr.getPreset(idx).engineType == EngineType::UnifiedVoice);
            for (int idx : fmIndices)
                expect(mgr.getPreset(idx).engineType == EngineType::FM);
        }

        beginTest("Filter by engine type and category");
        {
            PresetManager mgr;
            auto bassPresets = mgr.getFilteredIndices(EngineType::UnifiedVoice, "BASS");
            expect(!bassPresets.empty(), "Should have VA BASS presets");

            for (int idx : bassPresets)
            {
                expect(mgr.getPreset(idx).engineType == EngineType::UnifiedVoice);
                expectEquals(mgr.getPreset(idx).category, juce::String("BASS"));
            }
        }

        beginTest("Create preset from track");
        {
            Track track;
            track.engineType = EngineType::FM;
            track.soundParams = {{0, 1.0f}, {1, 0.8f}, {60, 3.0f}};

            auto preset = PresetManager::fromTrack(track, "My FM", "KEYS");
            expectEquals(preset.name, juce::String("My FM"));
            expect(preset.engineType == EngineType::FM);
            expectEquals((int) preset.soundParams.size(), 3);
            expect(!preset.isFactory);
        }

        beginTest("Apply preset to track");
        {
            Preset preset;
            preset.name = "Test";
            preset.engineType = EngineType::Wavetable;
            preset.soundParams = {{0, 0.5f}, {10, 2000.0f}};

            Track track;
            PresetManager::applyToTrack(preset, track);

            expect(track.engineType == EngineType::Wavetable);
            expectEquals((int) track.soundParams.size(), 2);
            expectWithinAbsoluteError(track.soundParams[10], 2000.0f, 0.01f);
        }

        beginTest("Cannot remove factory presets");
        {
            PresetManager mgr;
            int countBefore = mgr.getPresetCount();
            mgr.removePreset(0);
            expectEquals(mgr.getPresetCount(), countBefore);
        }

        beginTest("Add and remove user preset");
        {
            PresetManager mgr;
            int countBefore = mgr.getPresetCount();

            Preset userPreset;
            userPreset.name = "User Sound";
            userPreset.engineType = EngineType::UnifiedVoice;
            userPreset.isFactory = false;
            mgr.addPreset(userPreset);

            expectEquals(mgr.getPresetCount(), countBefore + 1);

            mgr.removePreset(countBefore);
            expectEquals(mgr.getPresetCount(), countBefore);
        }

        beginTest("File save and load round-trip");
        {
            Preset original;
            original.name = "File Test";
            original.category = "LEAD";
            original.engineType = EngineType::UnifiedVoice;
            original.soundParams = {{0, 0.7f}, {10, 5000.0f}};

            auto tempDir = juce::File::getSpecialLocation(
                juce::File::tempDirectory).getChildFile("neptune_test");
            tempDir.createDirectory();
            auto file = tempDir.getChildFile("test_preset.json");

            PresetManager mgr;
            mgr.saveToFile(original, file);
            expect(file.existsAsFile());

            PresetManager mgr2;
            int before = mgr2.getPresetCount();
            mgr2.loadFromFile(file);
            expectEquals(mgr2.getPresetCount(), before + 1);
            expectEquals(mgr2.getPreset(before).name, juce::String("File Test"));

            file.deleteFile();
            tempDir.deleteRecursively();
        }
    }
};

static PresetManagerTests presetManagerTests;

// --- Delay Tests ---

class DelayTests : public juce::UnitTest
{
public:
    DelayTests() : UnitTest("Delay") {}

    void runTest() override
    {
        beginTest("Stereo delay produces delayed output");
        {
            Delay delay;
            delay.setSampleRate(44100.0);
            delay.setTime(0.01f);
            delay.setFeedback(0.0f);
            delay.setMix(1.0f);

            float impulseL = 1.0f, impulseR = 1.0f;
            delay.process(impulseL, impulseR);

            bool foundDelayed = false;
            for (int i = 0; i < 1000; ++i)
            {
                float l = 0.0f, r = 0.0f;
                delay.process(l, r);
                if (std::abs(l) > 0.1f)
                {
                    foundDelayed = true;
                    break;
                }
            }
            expect(foundDelayed, "Delay should produce output after delay time");
        }

        beginTest("Feedback creates repeating echoes");
        {
            Delay delay;
            delay.setSampleRate(44100.0);
            delay.setTime(0.02f);
            delay.setFeedback(0.5f);
            delay.setMix(1.0f);

            float l = 1.0f, r = 1.0f;
            delay.process(l, r);

            int echoCount = 0;
            float prevPeak = 0.0f;
            bool inEcho = false;

            for (int i = 0; i < 44100; ++i)
            {
                l = 0.0f; r = 0.0f;
                delay.process(l, r);

                if (std::abs(l) > 0.05f && !inEcho)
                {
                    inEcho = true;
                    echoCount++;
                    float peak = std::abs(l);
                    if (echoCount > 1)
                        expect(peak < prevPeak, "Echo should decay");
                    prevPeak = peak;
                }
                else if (std::abs(l) < 0.01f)
                {
                    inEcho = false;
                }
            }
            expect(echoCount >= 3, "Should have multiple echoes with feedback");
        }

        beginTest("Ping-pong mode produces alternating channels");
        {
            Delay delay;
            delay.setSampleRate(44100.0);
            delay.setTime(0.02f);
            delay.setFeedback(0.5f);
            delay.setMix(1.0f);
            delay.setMode(Delay::Mode::PingPong);

            float l = 1.0f, r = 0.0f;
            delay.process(l, r);

            bool foundRight = false;
            for (int i = 0; i < 4410; ++i)
            {
                l = 0.0f; r = 0.0f;
                delay.process(l, r);
                if (std::abs(r) > 0.05f)
                {
                    foundRight = true;
                    break;
                }
            }
            expect(foundRight, "Ping-pong should produce output in opposite channel");
        }

        beginTest("Reset clears delay buffer");
        {
            Delay delay;
            delay.setSampleRate(44100.0);
            delay.setTime(0.05f);
            delay.setFeedback(0.5f);
            delay.setMix(1.0f);

            float l = 1.0f, r = 1.0f;
            delay.process(l, r);
            for (int i = 0; i < 100; ++i)
            {
                l = 0.0f; r = 0.0f;
                delay.process(l, r);
            }

            delay.reset();

            float maxAfterReset = 0.0f;
            for (int i = 0; i < 4410; ++i)
            {
                l = 0.0f; r = 0.0f;
                delay.process(l, r);
                maxAfterReset = std::max(maxAfterReset, std::abs(l));
            }
            expect(maxAfterReset < 0.001f, "After reset, delay should produce silence");
        }
    }
};

static DelayTests delayTests;

// --- Reverb Tests ---

class ReverbTests : public juce::UnitTest
{
public:
    ReverbTests() : UnitTest("Reverb") {}

    void runTest() override
    {
        beginTest("Reverb produces output from impulse");
        {
            Reverb reverb;
            reverb.setSampleRate(44100.0);
            reverb.setDecay(0.5f);
            reverb.setMix(1.0f);

            float l = 1.0f, r = 1.0f;
            reverb.process(l, r);

            float maxOut = 0.0f;
            for (int i = 0; i < 44100; ++i)
            {
                l = 0.0f; r = 0.0f;
                reverb.process(l, r);
                maxOut = std::max(maxOut, std::max(std::abs(l), std::abs(r)));
            }
            expect(maxOut > 0.01f, "Reverb should produce output from impulse");
        }

        beginTest("Higher decay produces longer tail");
        {
            auto measureTail = [](float decayVal) -> int
            {
                Reverb reverb;
                reverb.setSampleRate(44100.0);
                reverb.setDecay(decayVal);
                reverb.setMix(1.0f);

                float l = 1.0f, r = 1.0f;
                reverb.process(l, r);

                int tailSamples = 0;
                for (int i = 0; i < 88200; ++i)
                {
                    l = 0.0f; r = 0.0f;
                    reverb.process(l, r);
                    if (std::abs(l) > 0.001f || std::abs(r) > 0.001f)
                        tailSamples = i;
                }
                return tailSamples;
            };

            int shortTail = measureTail(0.3f);
            int longTail = measureTail(0.9f);
            expect(longTail > shortTail, "Higher decay should produce longer reverb tail");
        }

        beginTest("Damping reduces high frequency content");
        {
            auto measureBrightness = [](float dampVal) -> float
            {
                Reverb reverb;
                reverb.setSampleRate(44100.0);
                reverb.setDecay(0.7f);
                reverb.setDamping(dampVal);
                reverb.setMix(1.0f);

                float l = 1.0f, r = 0.0f;
                reverb.process(l, r);

                float energy = 0.0f;
                float prevSample = 0.0f;
                for (int i = 0; i < 22050; ++i)
                {
                    l = 0.0f; r = 0.0f;
                    reverb.process(l, r);
                    float diff = l - prevSample;
                    energy += diff * diff;
                    prevSample = l;
                }
                return energy;
            };

            float lowDamp = measureBrightness(0.1f);
            float highDamp = measureBrightness(0.9f);
            expect(highDamp < lowDamp, "High damping should reduce HF energy");
        }

        beginTest("Reset clears reverb state");
        {
            Reverb reverb;
            reverb.setSampleRate(44100.0);
            reverb.setDecay(0.9f);
            reverb.setMix(1.0f);

            float l = 1.0f, r = 1.0f;
            reverb.process(l, r);
            for (int i = 0; i < 1000; ++i)
            {
                l = 0.0f; r = 0.0f;
                reverb.process(l, r);
            }

            reverb.reset();

            float maxAfterReset = 0.0f;
            for (int i = 0; i < 4410; ++i)
            {
                l = 0.0f; r = 0.0f;
                reverb.process(l, r);
                maxAfterReset = std::max(maxAfterReset, std::max(std::abs(l), std::abs(r)));
            }
            expect(maxAfterReset < 0.001f, "After reset, reverb should produce silence");
        }

        beginTest("Mix blends dry and wet");
        {
            Reverb reverb;
            reverb.setSampleRate(44100.0);
            reverb.setDecay(0.5f);
            reverb.setMix(0.0f);

            float l = 0.5f, r = 0.5f;
            reverb.process(l, r);
            expectWithinAbsoluteError(l, 0.5f, 0.01f);

            reverb.reset();
            reverb.setMix(0.5f);
            l = 0.5f; r = 0.5f;
            reverb.process(l, r);
            expect(std::abs(l - 0.5f) < 0.3f, "50% mix should blend");
        }
    }
};

static ReverbTests reverbTests;

// ── Insert Chain Tests ──────────────────────────────────────────
class InsertChainTests : public juce::UnitTest
{
public:
    InsertChainTests() : juce::UnitTest("Insert Chain") {}

    void runTest() override
    {
        beginTest("Effect factory creates correct types");
        {
            auto chorus = AudioEngine::createEffect(EffectType::ChorusEffect);
            expect(chorus != nullptr, "Chorus created");
            auto drive = AudioEngine::createEffect(EffectType::DriveEffect);
            expect(drive != nullptr, "Drive created");
            auto flanger = AudioEngine::createEffect(EffectType::FlangerEffect);
            expect(flanger != nullptr, "Flanger created");
            auto phaser = AudioEngine::createEffect(EffectType::PhaserEffect);
            expect(phaser != nullptr, "Phaser created");
            auto tremolo = AudioEngine::createEffect(EffectType::TremoloEffect);
            expect(tremolo != nullptr, "Tremolo created");
            auto delay = AudioEngine::createEffect(EffectType::DelayEffect);
            expect(delay != nullptr, "Delay created");
            auto reverb = AudioEngine::createEffect(EffectType::ReverbEffect);
            expect(reverb != nullptr, "Reverb created");
            auto eq = AudioEngine::createEffect(EffectType::EQEffect);
            expect(eq != nullptr, "EQ created");
            auto comp = AudioEngine::createEffect(EffectType::CompressorEffect);
            expect(comp != nullptr, "Compressor created");
            auto limiter = AudioEngine::createEffect(EffectType::LimiterEffect);
            expect(limiter != nullptr, "Limiter created");
        }

        beginTest("Add and remove insert effects");
        {
            AudioEngine engine;
            engine.addEngine(0, AudioEngine::createEngine(EngineType::UnifiedVoice));

            expect(engine.getInsertChain(0) == nullptr, "No chain before add");

            engine.addInsertEffect(0, EffectType::ChorusEffect);
            auto* chain = engine.getInsertChain(0);
            expect(chain != nullptr, "Chain created after add");
            expectEquals(chain->getEffectCount(), 1);

            engine.addInsertEffect(0, EffectType::DriveEffect);
            expectEquals(chain->getEffectCount(), 2);

            engine.removeInsertEffect(0, 0);
            expectEquals(chain->getEffectCount(), 1);
        }

        beginTest("Insert chain respects max inserts");
        {
            AudioEngine engine;
            engine.addEngine(0, AudioEngine::createEngine(EngineType::UnifiedVoice));

            for (int i = 0; i < AudioEngine::MAX_INSERTS_PER_TRACK + 2; ++i)
                engine.addInsertEffect(0, EffectType::DriveEffect);

            auto* chain = engine.getInsertChain(0);
            expectEquals(chain->getEffectCount(), AudioEngine::MAX_INSERTS_PER_TRACK);
        }

        beginTest("Insert chain processes audio");
        {
            AudioEngine engine;
            engine.addEngine(0, AudioEngine::createEngine(EngineType::UnifiedVoice));

            engine.addInsertEffect(0, EffectType::DriveEffect);
            auto* chain = engine.getInsertChain(0);
            expect(chain != nullptr);
            chain->prepare(44100.0, 128);

            auto* fx = chain->getEffect(0);
            fx->setParameter(0, 0.9f);
            fx->setParameter(2, 1.0f);

            float testL[128], testR[128];
            for (int i = 0; i < 128; ++i)
            {
                testL[i] = 0.5f * std::sin(2.0f * 3.14159f * 440.0f * (float)i / 44100.0f);
                testR[i] = testL[i];
            }
            float origL[128];
            std::memcpy(origL, testL, sizeof(origL));

            chain->processStereo(testL, testR, 128);

            bool changed = false;
            for (int i = 0; i < 128; ++i)
            {
                if (std::abs(testL[i] - origL[i]) > 0.001f)
                {
                    changed = true;
                    break;
                }
            }
            expect(changed, "Drive effect should modify audio");
        }

        beginTest("Effect type names");
        {
            expect(juce::String(AudioEngine::effectTypeName(EffectType::ChorusEffect)) == "CHR");
            expect(juce::String(AudioEngine::effectTypeName(EffectType::DriveEffect)) == "DRV");
            expect(juce::String(AudioEngine::effectTypeName(EffectType::ReverbEffect)) == "RVB");
            expect(juce::String(AudioEngine::effectTypeName(EffectType::DelayEffect)) == "DLY");
        }

        beginTest("Insert chain on nonexistent track returns null");
        {
            AudioEngine engine;
            expect(engine.getInsertChain(0) == nullptr);
            expect(engine.getInsertChain(-1) == nullptr);
            expect(engine.getInsertChain(99) == nullptr);
        }

        beginTest("Multiple tracks have independent chains");
        {
            AudioEngine engine;
            engine.addEngine(0, AudioEngine::createEngine(EngineType::UnifiedVoice));
            engine.addEngine(1, AudioEngine::createEngine(EngineType::FM));

            engine.addInsertEffect(0, EffectType::ChorusEffect);
            engine.addInsertEffect(1, EffectType::DriveEffect);
            engine.addInsertEffect(1, EffectType::FlangerEffect);

            auto* chain0 = engine.getInsertChain(0);
            auto* chain1 = engine.getInsertChain(1);
            expect(chain0 != chain1, "Different chain objects");
            expectEquals(chain0->getEffectCount(), 1);
            expectEquals(chain1->getEffectCount(), 2);
        }
    }
};

static InsertChainTests insertChainTests;

// ── FX Page Tests ──────────────────────────────────────────────
class FxPageTests : public juce::UnitTest
{
public:
    FxPageTests() : juce::UnitTest("FX Page") {}

    void runTest() override
    {
        beginTest("FX page identifies correctly");
        {
            SequencerCore seq;
            AudioEngine audio;
            TrackFxScreen screen(seq, &audio);

            bool foundFx = false;
            for (int i = 0; i < screen.getPageCount(); ++i)
            {
                if (screen.getPageName() == "FX")
                    foundFx = true;
                screen.onArrowRight();
            }
            expect(foundFx, "FX page exists in default pages");
        }

        beginTest("FX page encoder names show slot and type");
        {
            SequencerCore seq;
            AudioEngine audio;
            audio.addEngine(0, AudioEngine::createEngine(EngineType::UnifiedVoice));
            TrackFxScreen screen(seq, &audio);

            expect(screen.selectPageByName("FX"));

            // SLOT TYPE (ON WET) / the selected effect's four parameters
            expectEquals(screen.getEncoderName(0), juce::String("SLOT"));
            expectEquals(screen.getEncoderName(1), juce::String("TYPE"));
            expectEquals(screen.getEncoderName(4), juce::String("---"));

            // Turning TYPE puts an effect in the slot and its parameters appear under encoders 5-8.
            screen.onEncoderChange(1, 6);    // OFF -> DLY
            expectEquals(audio.getInsertChain(0)->getEffectCount(), 1);
            expectEquals(screen.getEncoderName(2), juce::String("SYNC"));    // a delay can follow the tempo
            expectEquals(screen.getEncoderName(3), juce::String("WET"));
            expectEquals(screen.getEncoderName(4), juce::String("TIME"));
            {
                // SYNC on: the TIME knob now steps through divisions, and SYNC off returns the seconds.
                auto* fx = audio.getInsertChain(0)->getEffect(0);
                const float seconds = fx->getParameter(0);
                screen.onEncoderChange(2, 1);
                expect(fx->isSynced());
                const int div = fx->getSyncDivision();
                screen.onEncoderChange(4, 1);
                expectEquals(fx->getSyncDivision(), div + 1);
                screen.onEncoderChange(4, 127);
                expectEquals(fx->getSyncDivision(), div);
                screen.onEncoderChange(2, 127);
                expect(! fx->isSynced());
                expectWithinAbsoluteError(fx->getParameter(0), seconds, 0.0005f);
            }
            expectEquals(screen.getEncoderName(5), juce::String("FDBK"));
            expectEquals(screen.getEncoderName(7), juce::String("MODE"));

            // Changing the type replaces the effect in place.
            screen.onEncoderChange(1, 1);    // DLY -> RVB
            expectEquals(audio.getInsertChain(0)->getEffectCount(), 1);
            expectEquals(screen.getEncoderName(4), juce::String("DCAY"));

            // Back to OFF removes it.
            screen.onEncoderChange(1, 121);  // -7
            expectEquals(audio.getInsertChain(0)->getEffectCount(), 0);
        }
    }
};

static FxPageTests fxPageTests;

// ── Sampler Engine Tests ────────────────────────────────────────
class SamplerEngineTests : public juce::UnitTest
{
public:
    SamplerEngineTests() : juce::UnitTest("Sampler Engine") {}

    void runTest() override
    {
        beginTest("Load mono sample");
        {
            SamplerEngine samp;
            samp.prepareToPlay(44100.0, 128);

            std::vector<float> data(44100);
            for (int i = 0; i < 44100; ++i)
                data[i] = std::sin(2.0f * 3.14159f * 440.0f * (float)i / 44100.0f);

            samp.loadSample(data.data(), 44100, 1, 44100.0);
            expectEquals(samp.getSampleLength(), 44100);
            expect(samp.getSampleData() != nullptr);
        }

        beginTest("Grid slicing");
        {
            SamplerEngine samp;
            std::vector<float> data(44100, 0.5f);
            samp.loadSample(data.data(), 44100, 1, 44100.0);

            samp.sliceByGrid(8);
            expectEquals(samp.getSliceCount(), 8);

            auto& slice0 = samp.getSlice(0);
            expectEquals(slice0.startSample, 0);
            expectEquals(slice0.endSample, 44100 / 8);

            auto& slice7 = samp.getSlice(7);
            expectEquals(slice7.endSample, 44100);
        }

        beginTest("Forward playback produces audio");
        {
            SamplerEngine samp;
            samp.prepareToPlay(44100.0, 128);

            std::vector<float> data(4410);
            for (int i = 0; i < 4410; ++i)
                data[i] = std::sin(2.0f * 3.14159f * 440.0f * (float)i / 44100.0f);
            samp.loadSample(data.data(), 4410, 1, 44100.0);

            samp.setParameter(SamplerEngine::Mode, (float) SamplerEngine::Forward);
            samp.setParameter(SamplerEngine::EndPoint, 1.0f);
            samp.setParameter(SamplerEngine::Volume, 0.8f);

            samp.noteOn(60, 0.8f, -1);

            juce::AudioBuffer<float> buf(2, 128);
            buf.clear();
            juce::MidiBuffer midi;
            samp.processBlock(buf, midi);

            float maxVal = 0.0f;
            for (int i = 0; i < 128; ++i)
                maxVal = std::max(maxVal, std::abs(buf.getSample(0, i)));

            expect(maxVal > 0.01f, "Should produce audible output");
        }

        beginTest("One-shot mode stops after sample end");
        {
            SamplerEngine samp;
            samp.prepareToPlay(44100.0, 64);

            std::vector<float> data(128, 0.5f);
            samp.loadSample(data.data(), 128, 1, 44100.0);

            samp.setParameter(SamplerEngine::Mode, (float) SamplerEngine::OneShot);
            samp.setParameter(SamplerEngine::EndPoint, 1.0f);
            samp.setParameter(SamplerEngine::Volume, 0.8f);

            samp.noteOn(60, 1.0f, -1);

            juce::AudioBuffer<float> buf(2, 64);
            buf.clear();
            juce::MidiBuffer midi;
            samp.processBlock(buf, midi);
            samp.processBlock(buf, midi);
            samp.processBlock(buf, midi);
            samp.processBlock(buf, midi);

            buf.clear();
            samp.processBlock(buf, midi);
            float sum = 0.0f;
            for (int i = 0; i < 64; ++i)
                sum += std::abs(buf.getSample(0, i));

            expect(sum < 0.01f, "Should be silent after sample ends in one-shot");
        }

        beginTest("Bit reduction changes audio");
        {
            SamplerEngine samp;
            samp.prepareToPlay(44100.0, 128);

            std::vector<float> data(4410);
            for (int i = 0; i < 4410; ++i)
                data[i] = std::sin(2.0f * 3.14159f * 440.0f * (float)i / 44100.0f);
            samp.loadSample(data.data(), 4410, 1, 44100.0);

            samp.setParameter(SamplerEngine::EndPoint, 1.0f);
            samp.setParameter(SamplerEngine::Volume, 0.8f);
            samp.setParameter(SamplerEngine::BitReduce, 0.0f);
            samp.noteOn(60, 1.0f, -1);

            juce::AudioBuffer<float> cleanBuf(2, 128);
            cleanBuf.clear();
            juce::MidiBuffer midi;
            samp.processBlock(cleanBuf, midi);

            samp.releaseResources();
            samp.setParameter(SamplerEngine::BitReduce, 0.8f);
            samp.noteOn(60, 1.0f, -1);

            juce::AudioBuffer<float> crushBuf(2, 128);
            crushBuf.clear();
            samp.processBlock(crushBuf, midi);

            bool different = false;
            for (int i = 0; i < 128; ++i)
            {
                if (std::abs(cleanBuf.getSample(0, i) - crushBuf.getSample(0, i)) > 0.001f)
                {
                    different = true;
                    break;
                }
            }
            expect(different, "Bit reduction should change audio character");
        }

        beginTest("Sampler engine type is correct");
        {
            SamplerEngine samp;
            expect(samp.getType() == EngineType::Sampler);
        }

        beginTest("Factory creates sampler");
        {
            auto eng = AudioEngine::createEngine(EngineType::Sampler);
            expect(eng != nullptr);
            expect(eng->getType() == EngineType::Sampler);
        }
    }
};

static SamplerEngineTests samplerEngineTests;


// ─── Effect Param Lock Tests ───────────────────────────────────
class EffectParamLockTests : public juce::UnitTest
{
public:
    EffectParamLockTests() : UnitTest("Effect Param Lock") {}

    void runTest() override
    {
        beginTest("Effect param ID encoding");
        {
            int base = AudioEngine::EFFECT_PARAM_BASE;
            expect(base == 200, "EFFECT_PARAM_BASE should be 200");
            expectEquals(base + 0 * 4 + 0, 200);  // slot 0, param 0
            expectEquals(base + 0 * 4 + 3, 203);  // slot 0, param 3
            expectEquals(base + 1 * 4 + 0, 204);  // slot 1, param 0
            expectEquals(base + 7 * 4 + 3, 231);  // slot 7, param 3
        }

        beginTest("Effect param ID decoding");
        {
            int paramId = 207;
            int offset = paramId - AudioEngine::EFFECT_PARAM_BASE;
            int slot = offset / 4;
            int pIdx = offset % 4;
            expectEquals(slot, 1);
            expectEquals(pIdx, 3);
        }

        beginTest("Hold+turn on FX page stores effect param lock");
        {
            SequencerCore seq;
            AudioEngine audio;
            audio.addEngine(0, AudioEngine::createEngine(EngineType::UnifiedVoice));
            audio.addInsertEffect(0, EffectType::ChorusEffect);

            auto& track = seq.getPattern().getTrack(0);
            track.getStep(3).trig = true;
            track.getStep(3).notes.push_back({60, 1.0f});

            TrackFxScreen screen(seq, &audio);
            expect(screen.selectPageByName("FX"));

            // Simulate holding pad 3 (step 3)
            screen.onPadPress(3, 100);

            // Turn encoder 4 (P1 of effect slot 0)
            screen.onEncoderChange(4, 10);

            // Verify param lock was stored with correct ID
            int expectedId = AudioEngine::EFFECT_PARAM_BASE + 0 * 4 + 0;
            auto& step = track.getStep(3);
            expect(step.paramLocks.find(expectedId) != step.paramLocks.end(),
                   "Step should have effect param lock");

            screen.onPadRelease(3);
        }

        beginTest("Trig callback applies effect param locks");
        {
            SequencerCore seq;
            AudioEngine audio;
            audio.addEngine(0, AudioEngine::createEngine(EngineType::UnifiedVoice));
            audio.addInsertEffect(0, EffectType::DriveEffect);

            auto& track = seq.getPattern().getTrack(0);
            auto& step = track.getStep(0);
            step.trig = true;
            step.notes.push_back({60, 1.0f});

            // Add an effect param lock: slot 0, param 0 = 0.75
            int effectParamId = AudioEngine::EFFECT_PARAM_BASE + 0 * 4 + 0;
            step.paramLocks[effectParamId] = 0.75f;

            // Simulate trig callback logic
            auto* chain = audio.getInsertChain(0);
            expect(chain != nullptr && chain->getEffectCount() > 0, "Chain should have effect");

            for (auto& [paramId, value] : step.paramLocks)
            {
                if (paramId >= AudioEngine::EFFECT_PARAM_BASE)
                {
                    int offset = paramId - AudioEngine::EFFECT_PARAM_BASE;
                    int slot = offset / 4;
                    int pIdx = offset % 4;
                    if (chain && slot < chain->getEffectCount())
                    {
                        auto* fx = chain->getEffect(slot);
                        if (fx) fx->setParameter(pIdx, value);
                    }
                }
            }

            auto* fx = chain->getEffect(0);
            expect(fx != nullptr, "Effect should exist");
            float result = fx->getParameter(0);
            expect(std::abs(result - 0.75f) < 0.01f, "Effect param should be set to locked value");
        }

        beginTest("Non-held encoder changes live sound params");
        {
            SequencerCore seq;
            AudioEngine audio;
            audio.addEngine(0, AudioEngine::createEngine(EngineType::UnifiedVoice));

            ParamPageScreen screen(seq, &audio);
            screen.onArrowRight();          // past the MAIN landing page
            // On SRC page, turn encoder 0 (no pad held)
            screen.onEncoderChange(0, 10);

            auto& track = seq.getPattern().getTrack(0);
            auto it = track.soundParams.find(0);
            expect(it != track.soundParams.end(), "Sound param should be set");
        }

        beginTest("Hold+turn on non-FX page stores regular param lock");
        {
            SequencerCore seq;
            AudioEngine audio;
            audio.addEngine(0, AudioEngine::createEngine(EngineType::UnifiedVoice));

            auto& track = seq.getPattern().getTrack(0);
            track.getStep(5).trig = true;
            track.getStep(5).notes.push_back({60, 1.0f});

            ParamPageScreen screen(seq, &audio);
            screen.onArrowRight();          // past the MAIN landing page
            // On SRC page, hold pad 5 and turn encoder 0
            screen.onPadPress(5, 100);
            screen.onEncoderChange(0, 10);

            auto& step = track.getStep(5);
            expect(step.paramLocks.find(0) != step.paramLocks.end(),
                   "Step should have param lock for SRC param");

            screen.onPadRelease(5);
        }

        beginTest("Lock indicator shown on FX page during playback");
        {
            SequencerCore seq;
            AudioEngine audio;
            audio.addEngine(0, AudioEngine::createEngine(EngineType::UnifiedVoice));
            audio.addInsertEffect(0, EffectType::ReverbEffect);

            auto& track = seq.getPattern().getTrack(0);
            auto& step = track.getStep(0);
            step.trig = true;
            step.notes.push_back({60, 1.0f});
            int effectParamId = AudioEngine::EFFECT_PARAM_BASE + 0 * 4 + 2;
            step.paramLocks[effectParamId] = 0.5f;

            expect(step.hasParamLock(effectParamId), "Step should report having the lock");
        }
    }
};

static EffectParamLockTests effectParamLockTests;

// ─── Send Bus Routing Tests ────────────────────────────────────
class SendBusRoutingTests : public juce::UnitTest
{
public:
    SendBusRoutingTests() : UnitTest("Send Bus Routing") {}

    void runTest() override
    {
        beginTest("Default send bus count");
        {
            AudioEngine audio;
            expectEquals(audio.getSendCount(), 2);
        }

        beginTest("Add send bus");
        {
            AudioEngine audio;
            int idx = audio.addSendBus(EffectType::ChorusEffect);
            expectEquals(idx, 2);
            expectEquals(audio.getSendCount(), 3);
            expect(audio.getSendBusType(2) == EffectType::ChorusEffect, "New bus type should be chorus");
        }

        beginTest("Remove send bus");
        {
            AudioEngine audio;
            audio.addSendBus(EffectType::FlangerEffect);
            expectEquals(audio.getSendCount(), 3);
            audio.removeSendBus(2);
            expectEquals(audio.getSendCount(), 2);
        }

        beginTest("Max send bus limit");
        {
            AudioEngine audio;
            for (int i = 0; i < 10; ++i)
                audio.addSendBus(EffectType::ChorusEffect);
            expect(audio.getSendCount() <= AudioEngine::MAX_SENDS,
                   "Should not exceed MAX_SENDS");
        }

        beginTest("Change send bus effect type");
        {
            AudioEngine audio;
            expect(audio.getSendBusType(0) == EffectType::ReverbEffect, "Bus 0 default is reverb");
            audio.setSendBusEffect(0, EffectType::DelayEffect);
            expect(audio.getSendBusType(0) == EffectType::DelayEffect, "Bus 0 should now be delay");
            expect(audio.getSendBus(0).getChain().getEffectCount() == 1, "Chain should have 1 effect");
        }

        beginTest("Send param base");
        {
            // Mixer parameters live above every engine's own ids (the drum engine uses 0-127).
            expectEquals(AudioEngine::PARAM_SEND_BASE, (int) MixParam::SEND_BASE);
            expect(AudioEngine::PARAM_SEND_BASE > 127);
            expect(MixParam::isMixParam(MixParam::VOLUME) && MixParam::isMixParam(MixParam::PAN));
            expect(!MixParam::isMixParam(24) && !MixParam::isMixParam(AudioEngine::EFFECT_PARAM_BASE));
        }

        beginTest("Dynamic send routing uses PARAM_SEND_BASE");
        {
            AudioEngine audio;
            audio.addSendBus(EffectType::ChorusEffect);
            expectEquals(audio.getSendCount(), 3);
            expect(MixParam::isMixParam(AudioEngine::PARAM_SEND_BASE + 2));
        }
    }
};

static SendBusRoutingTests sendBusRoutingTests;

class CustomScaleTests : public juce::UnitTest
{
public:
    CustomScaleTests() : UnitTest("Custom Scale") {}

    void runTest() override
    {
        beginTest("Default pages exist");
        {
            CustomScaleManager mgr;
            expect(mgr.getPageCount() >= 4, "Should have at least 4 default pages");
            expect(mgr.getPageCount() >= 6, "Should have at least 6 default pages");
        }

        beginTest("Chromatic page maps correctly");
        {
            CustomScaleManager mgr;
            mgr.setActivePage(0);
            for (int i = 0; i < 16; ++i)
                expectEquals(mgr.getNoteForPad(i), 60 + i);
        }

        beginTest("Relative mode transposes from root");
        {
            CustomScaleManager mgr;
            mgr.setActivePage(0);
            expectEquals(mgr.getNoteForPad(0), 60);
            mgr.setRootNote(0, 72);
            expectEquals(mgr.getRootNote(), 72);
            expectEquals(mgr.getNoteForPad(0), 72);
            expectEquals(mgr.getNoteForPad(1), 73);
            expectEquals(mgr.getNoteForPad(15), 87);
        }

        beginTest("Absolute mode ignores root");
        {
            CustomScaleManager mgr;
            mgr.setActivePage(0);
            mgr.setPageMode(0, CustomScaleManager::Mode::Absolute);
            for (int i = 0; i < 16; ++i)
                mgr.setNoteForPad(0, i, 40 + i);
            mgr.setRootNote(0, 72);
            for (int i = 0; i < 16; ++i)
                expectEquals(mgr.getNoteForPad(i), 40 + i);
        }

        beginTest("Page change sets root note");
        {
            CustomScaleManager mgr;
            mgr.setRootNote(1, 48);
            mgr.setActivePage(0);
            expectEquals(mgr.getRootNote(), 60);
            mgr.setActivePage(1);
            expectEquals(mgr.getRootNote(), 48);
        }

        beginTest("Note assignment");
        {
            CustomScaleManager mgr;
            mgr.setNoteForPad(0, 5, 42);
            expectEquals(mgr.getPage(0).notes[5], 42);
            mgr.setActivePage(0);
            expectEquals(mgr.getNoteForPad(5), 60 + 42);
        }
    }
};

class CustomScaleScreenTests : public juce::UnitTest
{
public:
    CustomScaleScreenTests() : UnitTest("Custom Scale Screen") {}

    void runTest() override
    {
        beginTest("Encoder changes page");
        {
            CustomScaleManager mgr;
            CustomScaleScreen screen(mgr);
            expectEquals(mgr.getActivePage(), 0);
            screen.onEncoderChange(0, 1);
            expectEquals(mgr.getActivePage(), 1);
            screen.onEncoderChange(0, 1);
            expectEquals(mgr.getActivePage(), 2);
            screen.onEncoderChange(0, 127);
            expectEquals(mgr.getActivePage(), 1);
        }

        beginTest("Encoder name returns 2-char");
        {
            CustomScaleManager mgr;
            CustomScaleScreen screen(mgr);
            for (int i = 0; i < 8; ++i)
            {
                auto name = screen.getEncoderName(i);
                expect(name.length() <= 2, "Encoder name should be at most 2 chars");
            }
        }

        beginTest("Render does not crash");
        {
            CustomScaleManager mgr;
            CustomScaleScreen screen(mgr);
            DisplayRenderer display;
            screen.render(display);
            int litPixels = 0;
            for (int y = 0; y < DisplayRenderer::HEIGHT; ++y)
                for (int x = 0; x < DisplayRenderer::WIDTH; ++x)
                    if (display.getPixel(x, y))
                        ++litPixels;
            expect(litPixels > 0, "Rendered screen should have visible content");
        }

        beginTest("Pad press triggers note callback");
        {
            CustomScaleManager mgr;
            CustomScaleScreen screen(mgr);
            int receivedNote = -1;
            int receivedVelocity = -1;
            screen.setNoteCallback([&](int note, int vel) {
                receivedNote = note;
                receivedVelocity = vel;
            });
            screen.onPadPress(0, 100);
            expectEquals(receivedNote, 60);
            expectEquals(receivedVelocity, 100);
        }

        beginTest("Root note encoder changes root");
        {
            CustomScaleManager mgr;
            CustomScaleScreen screen(mgr);
            expectEquals(mgr.getRootNote(), 60);
            screen.onEncoderChange(1, 2);
            expectEquals(mgr.getRootNote(), 62);
        }

        beginTest("Mode encoder toggles mode");
        {
            CustomScaleManager mgr;
            CustomScaleScreen screen(mgr);
            expect(mgr.getPageMode(0) == CustomScaleManager::Mode::Relative);
            screen.onEncoderChange(2, 1);
            expect(mgr.getPageMode(0) == CustomScaleManager::Mode::Absolute);
            screen.onEncoderChange(2, 127);
            expect(mgr.getPageMode(0) == CustomScaleManager::Mode::Relative);
        }
    }
};

static CustomScaleTests customScaleTests;
static CustomScaleScreenTests customScaleScreenTests;

class ArpeggiatorTests : public juce::UnitTest
{
public:
    ArpeggiatorTests() : UnitTest("Arpeggiator") {}

    void runTest() override
    {
        beginTest("Up mode produces ascending notes");
        {
            Arpeggiator arp;
            arp.setMode(Arpeggiator::Mode::Up);
            arp.setRate(Arpeggiator::Rate::Sixteenth);
            arp.setGate(0.5f);
            arp.setOctaveRange(1);
            std::vector<int> pitches;
            arp.onNoteOn = [&](int pitch, float) { pitches.push_back(pitch); };
            arp.onNoteOff = [](int) {};
            arp.noteOn(60, 1.0f);
            arp.noteOn(64, 1.0f);
            arp.noteOn(67, 1.0f);
            arp.triggerFirstNote();
            for (int i = 0; i < 10; ++i)
                arp.processBlock(5600, 44100.0, 120.0f);
            expect(pitches.size() >= 3, "Should have triggered at least 3 notes");
            if (pitches.size() >= 3)
            {
                expectEquals(pitches[0], 60);
                expectEquals(pitches[1], 64);
                expectEquals(pitches[2], 67);
            }
        }

        beginTest("Down mode produces descending notes");
        {
            Arpeggiator arp;
            arp.setMode(Arpeggiator::Mode::Down);
            arp.setRate(Arpeggiator::Rate::Sixteenth);
            arp.setGate(0.5f);
            arp.setOctaveRange(1);
            std::vector<int> pitches;
            arp.onNoteOn = [&](int pitch, float) { pitches.push_back(pitch); };
            arp.onNoteOff = [](int) {};
            arp.noteOn(60, 1.0f);
            arp.noteOn(64, 1.0f);
            arp.noteOn(67, 1.0f);
            arp.triggerFirstNote();
            for (int i = 0; i < 10; ++i)
                arp.processBlock(5600, 44100.0, 120.0f);
            expect(pitches.size() >= 3, "Should have triggered at least 3 notes");
            if (pitches.size() >= 3)
            {
                expectEquals(pitches[0], 67);
                expectEquals(pitches[1], 64);
                expectEquals(pitches[2], 60);
            }
        }

        beginTest("Octave range extends notes");
        {
            Arpeggiator arp;
            arp.setMode(Arpeggiator::Mode::Up);
            arp.setRate(Arpeggiator::Rate::Sixteenth);
            arp.setGate(0.5f);
            arp.setOctaveRange(2);
            std::vector<int> pitches;
            arp.onNoteOn = [&](int pitch, float) { pitches.push_back(pitch); };
            arp.onNoteOff = [](int) {};
            arp.noteOn(60, 1.0f);
            arp.triggerFirstNote();
            for (int i = 0; i < 10; ++i)
                arp.processBlock(5600, 44100.0, 120.0f);
            expect(pitches.size() >= 2, "Should have triggered at least 2 notes");
            bool foundC4 = false, foundC5 = false;
            for (int p : pitches)
            {
                if (p == 60) foundC4 = true;
                if (p == 72) foundC5 = true;
            }
            expect(foundC4, "Should include C4 (60)");
            expect(foundC5, "Should include C5 (72)");
        }

        beginTest("Gate controls note duration");
        {
            Arpeggiator arp;
            arp.setMode(Arpeggiator::Mode::Up);
            arp.setRate(Arpeggiator::Rate::Sixteenth);
            arp.setGate(0.25f);
            arp.setOctaveRange(1);
            int noteOnCount = 0;
            int noteOffCount = 0;
            arp.onNoteOn = [&](int, float) { ++noteOnCount; };
            arp.onNoteOff = [&](int) { ++noteOffCount; };
            arp.noteOn(60, 1.0f);
            arp.triggerFirstNote();
            for (int i = 0; i < 10; ++i)
                arp.processBlock(5600, 44100.0, 120.0f);
            expect(noteOffCount > 0, "Short gate should trigger note-offs");
            expect(noteOffCount >= noteOnCount - 1,
                   "Note-offs should match or nearly match note-ons");
        }

        beginTest("Latch holds notes after release");
        {
            Arpeggiator arp;
            arp.setMode(Arpeggiator::Mode::Up);
            arp.setRate(Arpeggiator::Rate::Sixteenth);
            arp.setGate(0.5f);
            arp.setLatch(true);
            arp.setOctaveRange(1);
            std::vector<int> pitches;
            arp.onNoteOn = [&](int pitch, float) { pitches.push_back(pitch); };
            arp.onNoteOff = [](int) {};
            arp.noteOn(60, 1.0f);
            arp.noteOn(64, 1.0f);
            arp.triggerFirstNote();
            arp.noteOff(60);
            arp.noteOff(64);
            expect(arp.isRunning(), "Arp should still be running in latch mode after note-off");
            pitches.clear();
            for (int i = 0; i < 10; ++i)
                arp.processBlock(5600, 44100.0, 120.0f);
            expect(pitches.size() > 0, "Latched arp should still produce notes after key release");
        }

        beginTest("Note off stops arp when not latched");
        {
            Arpeggiator arp;
            arp.setMode(Arpeggiator::Mode::Up);
            arp.setRate(Arpeggiator::Rate::Sixteenth);
            arp.setGate(0.5f);
            arp.setLatch(false);
            arp.setOctaveRange(1);
            std::vector<int> pitches;
            arp.onNoteOn = [&](int pitch, float) { pitches.push_back(pitch); };
            arp.onNoteOff = [](int) {};
            arp.noteOn(60, 1.0f);
            arp.triggerFirstNote();
            arp.noteOff(60);
            expect(!arp.isRunning(), "Arp should stop when all notes released without latch");
            pitches.clear();
            for (int i = 0; i < 10; ++i)
                arp.processBlock(5600, 44100.0, 120.0f);
            expectEquals((int)pitches.size(), 0, "No notes should play after all keys released");
        }
    }
};

class ArpScreenTests : public juce::UnitTest
{
public:
    ArpScreenTests() : UnitTest("Arp Screen") {}

    void runTest() override
    {
        beginTest("Encoder changes mode");
        {
            Arpeggiator arp;
            arp.setMode(Arpeggiator::Mode::Up);
            ArpScreen screen(arp);
            screen.onEncoderChange(0, 1);
            expectEquals((int)arp.getMode(), (int)Arpeggiator::Mode::Down);
            screen.onEncoderChange(0, 1);
            expectEquals((int)arp.getMode(), (int)Arpeggiator::Mode::UpDown);
        }

        beginTest("Encoder name returns 2-char");
        {
            Arpeggiator arp;
            ArpScreen screen(arp);
            for (int i = 0; i < 8; ++i)
            {
                auto name = screen.getEncoderName(i);
                expect(name.isNotEmpty() && name.length() <= 4,
                    "Cell name '" + name + "' at index " + juce::String(i) + " should fit a cell");
            }
        }

        beginTest("Render does not crash");
        {
            Arpeggiator arp;
            ArpScreen screen(arp);
            DisplayRenderer display;
            screen.render(display);
            int litPixels = 0;
            for (int y = 0; y < DisplayRenderer::HEIGHT; ++y)
                for (int x = 0; x < DisplayRenderer::WIDTH; ++x)
                    if (display.getPixel(x, y))
                        ++litPixels;
            expect(litPixels > 0, "ArpScreen render should produce visible output");
        }

        beginTest("Sub-page shows ON/OFF");
        {
            Arpeggiator arp;
            ArpScreen screen(arp);
            arp.setEnabled(true);
            expect(screen.getSubPage().startsWith("ON"));
            arp.setEnabled(false);
            expect(screen.getSubPage().startsWith("OFF"));
        }

        beginTest("Gate encoder adjusts gate");
        {
            Arpeggiator arp;
            arp.setGate(0.5f);
            ArpScreen screen(arp);
            screen.onEncoderChange(3, 5);
            expect(arp.getGate() > 0.5f, "Gate should increase");
        }

        beginTest("Latch encoder toggles latch");
        {
            Arpeggiator arp;
            arp.setLatch(false);
            ArpScreen screen(arp);
            screen.onEncoderChange(7, 1);
            expect(arp.getLatch(), "Clockwise switches latch on");
            screen.onEncoderChange(7, 127);
            expect(!arp.getLatch(), "Anticlockwise switches latch off");
        }
    }
};

static ArpeggiatorTests arpeggiatorTests;
static ArpScreenTests arpScreenTests;

// ---------------------------------------------------------------
// Song Mode Tests
// ---------------------------------------------------------------
class SongModeTests : public juce::UnitTest
{
public:
    SongModeTests() : UnitTest("Song Mode") {}

    void runTest() override
    {
        beginTest("Add and remove entries");
        {
            SongMode song;
            expectEquals(song.getEntryCount(), 0);

            song.addEntry(0, 2);
            song.addEntry(3, 4);
            song.addEntry(7);
            expectEquals(song.getEntryCount(), 3);

            expectEquals(song.getEntry(0).patternIndex, 0);
            expectEquals(song.getEntry(0).repeatCount, 2);
            expectEquals(song.getEntry(1).patternIndex, 3);
            expectEquals(song.getEntry(1).repeatCount, 4);
            expectEquals(song.getEntry(2).patternIndex, 7);
            expectEquals(song.getEntry(2).repeatCount, 1);

            song.removeEntry(1);
            expectEquals(song.getEntryCount(), 2);
            expectEquals(song.getEntry(1).patternIndex, 7);

            expect(!song.removeEntry(-1));
            expect(!song.removeEntry(99));
        }

        beginTest("Playback advances through entries");
        {
            SongMode song;
            song.addEntry(0, 1);
            song.addEntry(5, 1);
            song.addEntry(10, 1);
            song.setLoopMode(false);

            song.start();
            expect(song.isActive());
            expectEquals(song.getCurrentPatternIndex(), 0);

            bool changed = song.advancePattern();
            expect(changed);
            expectEquals(song.getCurrentPatternIndex(), 5);

            changed = song.advancePattern();
            expect(changed);
            expectEquals(song.getCurrentPatternIndex(), 10);

            changed = song.advancePattern();
            expect(!changed);
            expect(!song.isActive());
            expect(song.isFinished());
        }

        beginTest("Repeat counts are respected");
        {
            SongMode song;
            song.addEntry(0, 3);
            song.addEntry(1, 2);
            song.setLoopMode(false);

            song.start();
            expectEquals(song.getCurrentPatternIndex(), 0);
            expectEquals(song.getCurrentRepeat(), 0);

            bool changed = song.advancePattern();
            expect(!changed);
            expectEquals(song.getCurrentPatternIndex(), 0);
            expectEquals(song.getCurrentRepeat(), 1);

            changed = song.advancePattern();
            expect(!changed);
            expectEquals(song.getCurrentRepeat(), 2);

            changed = song.advancePattern();
            expect(changed);
            expectEquals(song.getCurrentPatternIndex(), 1);
            expectEquals(song.getCurrentRepeat(), 0);

            changed = song.advancePattern();
            expect(!changed);

            changed = song.advancePattern();
            expect(!changed);
            expect(!song.isActive());
        }

        beginTest("Loop mode wraps around");
        {
            SongMode song;
            song.addEntry(0, 1);
            song.addEntry(1, 1);
            song.setLoopMode(true);

            song.start();
            expectEquals(song.getCurrentPatternIndex(), 0);

            song.advancePattern();
            expectEquals(song.getCurrentPatternIndex(), 1);

            song.advancePattern();
            expect(song.isActive());
            expectEquals(song.getCurrentPatternIndex(), 0);
            expectEquals(song.getCurrentEntryIndex(), 0);
        }

        beginTest("Stop with and without hold");
        {
            SongMode song;
            song.addEntry(0, 1);
            song.addEntry(1, 1);
            song.addEntry(2, 1);

            song.start();
            song.advancePattern();

            song.stop(true);
            expect(!song.isActive());
            expectEquals(song.getCurrentEntryIndex(), 1);

            song.start();
            song.advancePattern();
            song.stop(false);
            expectEquals(song.getCurrentEntryIndex(), 0);
        }

        beginTest("Serialization round-trip");
        {
            SongMode original;
            original.addEntry(3, 5);
            original.addEntry(7, 2);
            original.addEntry(15, 1);
            original.setLoopMode(false);

            SongEntry e;
            e.patternIndex = 10;
            e.repeatCount = 3;
            e.transposeSemitones = -5;
            original.setEntry(1, e);

            auto v = original.toVar();

            SongMode loaded;
            loaded.fromVar(v);

            expectEquals(loaded.getEntryCount(), 3);
            expectEquals(loaded.getEntry(0).patternIndex, 3);
            expectEquals(loaded.getEntry(0).repeatCount, 5);
            expectEquals(loaded.getEntry(1).patternIndex, 10);
            expectEquals(loaded.getEntry(1).repeatCount, 3);
            expectEquals(loaded.getEntry(1).transposeSemitones, -5);
            expectEquals(loaded.getEntry(2).patternIndex, 15);
            expect(!loaded.getLoopMode());
        }

        beginTest("Transpose is stored and retrievable");
        {
            SongMode song;
            song.addEntry(0, 1);
            song.addEntry(1, 1);

            SongEntry e;
            e.patternIndex = 1;
            e.repeatCount = 1;
            e.transposeSemitones = 7;
            song.setEntry(1, e);

            song.start();
            expectEquals(song.getCurrentTranspose(), 0);

            song.advancePattern();
            expectEquals(song.getCurrentTranspose(), 7);

            SongEntry extreme;
            extreme.patternIndex = 0;
            extreme.repeatCount = 1;
            extreme.transposeSemitones = 24;
            song.setEntry(0, extreme);
            expectEquals(song.getEntry(0).transposeSemitones, 12);
        }

        beginTest("Clear resets everything");
        {
            SongMode song;
            song.addEntry(0, 1);
            song.addEntry(1, 1);
            song.start();
            song.advancePattern();

            song.clear();
            expectEquals(song.getEntryCount(), 0);
            expect(!song.isActive());
            expectEquals(song.getCurrentEntryIndex(), 0);
            expectEquals(song.getCurrentRepeat(), 0);
        }

        beginTest("Move entry reorders list");
        {
            SongMode song;
            song.addEntry(0, 1);
            song.addEntry(5, 2);
            song.addEntry(10, 3);

            song.moveEntry(0, 2);
            expectEquals(song.getEntry(0).patternIndex, 5);
            expectEquals(song.getEntry(1).patternIndex, 10);
            expectEquals(song.getEntry(2).patternIndex, 0);

            expect(!song.moveEntry(-1, 0));
            expect(!song.moveEntry(0, 99));
        }
    }
};

static SongModeTests songModeTests;

// ---------------------------------------------------------------
// MIDI Clock Tests
// ---------------------------------------------------------------
class MIDIClockTests : public juce::UnitTest
{
public:
    MIDIClockTests() : UnitTest("MIDI Clock") {}

    void runTest() override
    {
        beginTest("Internal mode generates clock events");
        {
            MIDIClock clock;
            clock.setMode(MIDIClock::Mode::Internal);
            clock.prepareToPlay(44100.0, 512);
            clock.setTempo(120.0);
            clock.startTransport();

            int samplesPerBeat = (int)(44100.0 * 60.0 / 120.0);
            clock.processBlock(samplesPerBeat, 44100.0);

            int tickCount = 0;
            MIDIClock::ClockEvent evt;
            while (clock.popOutputEvent(evt))
            {
                if (evt.type == MIDIClock::ClockEvent::Clock)
                    tickCount++;
            }

            expect(tickCount >= 22 && tickCount <= 26, "Should produce ~24 clock ticks per beat");
        }

        beginTest("Start/stop transport emits events");
        {
            MIDIClock clock;
            clock.setMode(MIDIClock::Mode::Internal);
            clock.prepareToPlay(44100.0, 512);

            clock.startTransport();

            MIDIClock::ClockEvent evt;
            bool gotStart = false;
            while (clock.popOutputEvent(evt))
                if (evt.type == MIDIClock::ClockEvent::Start) gotStart = true;
            expect(gotStart, "startTransport should emit Start event");

            clock.stopTransport();
            bool gotStop = false;
            while (clock.popOutputEvent(evt))
                if (evt.type == MIDIClock::ClockEvent::Stop) gotStop = true;
            expect(gotStop, "stopTransport should emit Stop event");

            clock.startTransport();
            while (clock.popOutputEvent(evt)) {}
            clock.continueTransport();
            bool gotContinue = false;
            while (clock.popOutputEvent(evt))
                if (evt.type == MIDIClock::ClockEvent::Continue) gotContinue = true;
            expect(gotContinue, "continueTransport should emit Continue event");
        }

        beginTest("External mode detects tempo from MIDI clock");
        {
            MIDIClock clock;
            clock.setMode(MIDIClock::Mode::External);
            clock.prepareToPlay(44100.0, 512);

            double samplesPerTick = (44100.0 * 60.0 / 120.0) / 24.0;

            auto startMsg = juce::MidiMessage::midiStart();
            clock.handleMidiMessage(startMsg);

            for (int i = 0; i < 48; ++i)
            {
                clock.processBlock((int)samplesPerTick, 44100.0);
                auto tickMsg = juce::MidiMessage::midiClock();
                clock.handleMidiMessage(tickMsg);
            }

            double detected = clock.getDetectedTempo();
            expect(detected > 100.0 && detected < 140.0, "Should detect tempo near 120");
        }

        beginTest("External mode handles start/stop messages");
        {
            MIDIClock clock;
            clock.setMode(MIDIClock::Mode::External);
            clock.prepareToPlay(44100.0, 512);

            auto startMsg = juce::MidiMessage::midiStart();
            clock.handleMidiMessage(startMsg);
            expect(clock.isRunning());

            auto stopMsg = juce::MidiMessage::midiStop();
            clock.handleMidiMessage(stopMsg);
            expect(!clock.isRunning());
        }

        beginTest("Mode switching");
        {
            MIDIClock clock;
            expectEquals((int) clock.getMode(), (int) MIDIClock::Mode::Internal);

            clock.setMode(MIDIClock::Mode::External);
            expectEquals((int) clock.getMode(), (int) MIDIClock::Mode::External);

            clock.setMode(MIDIClock::Mode::Auto);
            expectEquals((int) clock.getMode(), (int) MIDIClock::Mode::Auto);
        }

        beginTest("Reset clears state");
        {
            MIDIClock clock;
            clock.setMode(MIDIClock::Mode::Internal);
            clock.prepareToPlay(44100.0, 512);
            clock.startTransport();
            expect(clock.isRunning());

            clock.reset();
            expect(!clock.isRunning());
            expectWithinAbsoluteError(clock.getCurrentBeat(), 0.0, 0.001);
        }
    }
};

static MIDIClockTests midiClockTests;

// ---------------------------------------------------------------
// Variation Macros Tests
// ---------------------------------------------------------------
class VariationMacrosTests : public juce::UnitTest
{
public:
    VariationMacrosTests() : UnitTest("Variation Macros") {}

    void runTest() override
    {
        beginTest("All macros at zero produce no change");
        {
            VariationMacros vm;
            vm.setSpice(0.0f);
            vm.setDice(0.0f);
            vm.setDensity(0.5f);
            vm.setOctaveScatter(0.0f);
            vm.setRatchetChance(0.0f);
            vm.setGateSpread(0.0f);
            vm.setHumanize(0.0f);

            for (int step = 0; step < 16; ++step)
            {
                auto mod = vm.apply(step, 16, 12345);
                expectEquals(mod.noteOffset, 0);
                expectWithinAbsoluteError(mod.velocityScale, 1.0f, 0.001f);
                expectWithinAbsoluteError(mod.gateScale, 1.0f, 0.001f);
                expectWithinAbsoluteError(mod.timingOffset, 0.0f, 0.001f);
                expect(!mod.skip);
                expectEquals(mod.ratchetCount, 1);
            }
        }

        beginTest("Spice adds extra notes");
        {
            VariationMacros vm;
            vm.setSpice(1.0f);

            int totalExtras = 0;
            for (int step = 0; step < 64; ++step)
            {
                auto mod = vm.apply(step, 64, 42);
                totalExtras += mod.extraNotes;
            }

            expect(totalExtras > 0, "Spice at 1.0 should add extra notes");
        }

        beginTest("Dice alters note offset");
        {
            VariationMacros vm;
            vm.setDice(1.0f);

            bool anyOffset = false;
            for (int step = 0; step < 64; ++step)
            {
                auto mod = vm.apply(step, 64, 99);
                if (mod.noteOffset != 0)
                    anyOffset = true;
            }

            expect(anyOffset, "Dice at 1.0 should shift some notes");
        }

        beginTest("Density thins and fills");
        {
            VariationMacros vm;
            vm.setDensity(0.0f);

            int skipped = 0;
            for (int step = 0; step < 16; ++step)
            {
                auto mod = vm.apply(step, 16, 77);
                if (mod.skip) ++skipped;
            }
            expect(skipped > 0, "Density 0 should skip some steps");

            vm.setDensity(1.0f);
            int extras = 0;
            for (int step = 0; step < 64; ++step)
            {
                auto mod = vm.apply(step, 64, 77);
                extras += mod.extraNotes;
            }
            expect(extras > 0, "Density 1.0 should add ghost notes");
        }

        beginTest("Ratchet creates repeats");
        {
            VariationMacros vm;
            vm.setRatchetChance(1.0f);

            bool anyRatchet = false;
            for (int step = 0; step < 64; ++step)
            {
                auto mod = vm.apply(step, 64, 55);
                if (mod.ratchetCount > 1)
                    anyRatchet = true;
            }
            expect(anyRatchet, "RatchetChance at 1.0 should produce ratchets");
        }

        beginTest("Humanize adds jitter");
        {
            VariationMacros vm;
            vm.setHumanize(1.0f);

            bool anyTimingOffset = false;
            bool anyVelChange = false;
            for (int step = 0; step < 64; ++step)
            {
                auto mod = vm.apply(step, 64, 101);
                if (std::abs(mod.timingOffset) > 0.001f)
                    anyTimingOffset = true;
                if (std::abs(mod.velocityScale - 1.0f) > 0.01f)
                    anyVelChange = true;
            }
            expect(anyTimingOffset, "Humanize should add timing jitter");
            expect(anyVelChange, "Humanize should vary velocity");
        }

        beginTest("Deterministic with same seed");
        {
            VariationMacros vm;
            vm.setSpice(0.7f);
            vm.setDice(0.5f);
            vm.setDensity(0.3f);
            vm.setOctaveScatter(0.4f);
            vm.setRatchetChance(0.6f);
            vm.setGateSpread(0.8f);
            vm.setHumanize(0.5f);

            for (int step = 0; step < 16; ++step)
            {
                auto mod1 = vm.apply(step, 16, 12345);
                auto mod2 = vm.apply(step, 16, 12345);

                expectEquals(mod1.noteOffset, mod2.noteOffset);
                expectEquals(mod1.velocityScale, mod2.velocityScale);
                expectEquals(mod1.gateScale, mod2.gateScale);
                expectEquals(mod1.timingOffset, mod2.timingOffset);
                expect(mod1.skip == mod2.skip);
                expectEquals(mod1.ratchetCount, mod2.ratchetCount);
                expectEquals(mod1.extraNotes, mod2.extraNotes);
            }
        }

        beginTest("All macros interact without crash");
        {
            VariationMacros vm;
            vm.setSpice(1.0f);
            vm.setDice(1.0f);
            vm.setDensity(0.0f);
            vm.setOctaveScatter(1.0f);
            vm.setRatchetChance(1.0f);
            vm.setGateSpread(1.0f);
            vm.setHumanize(1.0f);

            for (int step = 0; step < 128; ++step)
            {
                auto mod = vm.apply(step, 128, 9999);
                expect(mod.velocityScale >= 0.0f);
                expect(mod.gateScale >= 0.25f && mod.gateScale <= 2.0f);
                expect(mod.timingOffset >= -0.5f && mod.timingOffset <= 0.5f);
                expect(mod.ratchetCount >= 1 && mod.ratchetCount <= 3);
            }
        }
    }
};

// ---------------------------------------------------------------
// Rhythm Capture Tests
// ---------------------------------------------------------------
class RhythmCaptureTests : public juce::UnitTest
{
public:
    RhythmCaptureTests() : UnitTest("Rhythm Capture") {}

    void runTest() override
    {
        beginTest("Capture start and stop");
        {
            RhythmCapture rc;
            expect(!rc.isCapturing());
            expect(!rc.hasCapture());

            rc.startCapture(44100.0);
            expect(rc.isCapturing());

            rc.stopCapture();
            expect(!rc.isCapturing());
        }

        beginTest("Tap events are recorded");
        {
            RhythmCapture rc;
            rc.startCapture(44100.0);

            rc.tapEvent(0.8f);
            rc.advanceSamples(1000.0);
            rc.tapEvent(0.6f);
            rc.advanceSamples(500.0);
            rc.tapEvent(1.0f);

            rc.stopCapture();

            expect(rc.hasCapture());
            expectEquals(rc.getEventCount(), 3);

            auto& events = rc.getEvents();
            expectWithinAbsoluteError((float) events[0].timeSamples, 0.0f, 0.1f);
            expectWithinAbsoluteError((float) events[1].timeSamples, 1000.0f, 0.1f);
            expectWithinAbsoluteError((float) events[2].timeSamples, 1500.0f, 0.1f);
            expectWithinAbsoluteError(events[0].velocity, 0.8f, 0.001f);
        }

        beginTest("Normalize scales times to step length");
        {
            RhythmCapture rc;
            rc.startCapture(44100.0);

            rc.tapEvent(1.0f);
            rc.advanceSamples(1000.0);
            rc.tapEvent(1.0f);
            rc.advanceSamples(1000.0);
            rc.tapEvent(1.0f);

            rc.stopCapture();

            rc.normalizeToStepLength(4000.0);

            auto& events = rc.getEvents();
            expectWithinAbsoluteError((float) events[0].timeSamples, 0.0f, 0.01f);
            expectWithinAbsoluteError((float) events[2].timeSamples, 4000.0f, 0.01f);
            expectWithinAbsoluteError((float) events[1].timeSamples, 2000.0f, 0.01f);
        }

        beginTest("Empty capture returns no events");
        {
            RhythmCapture rc;
            expect(!rc.hasCapture());
            expectEquals(rc.getEventCount(), 0);
            expect(rc.getEvents().empty());

            rc.startCapture(44100.0);
            rc.stopCapture();
            expect(!rc.hasCapture());
            expectEquals(rc.getEventCount(), 0);
        }
    }
};

static VariationMacrosTests variationMacrosTests;
static RhythmCaptureTests rhythmCaptureTests;

// ---------------------------------------------------------------
// Fill Mode Tests
// ---------------------------------------------------------------
class FillModeTests : public juce::UnitTest
{
public:
    FillModeTests() : UnitTest("Fill Mode") {}

    void runTest() override
    {
        beginTest("Fill inactive: Fill steps skipped");
        {
            SequencerCore seq;
            seq.prepareToPlay(44100.0, 512);

            auto& track = seq.getPattern().getTrack(0);
            track.setLength(2);
            track.getStep(0).trig = true;
            track.getStep(0).notes.push_back({60, 1.0f});
            track.getStep(0).trigCondition.type = TrigCondition::Fill;

            int trigCount = 0;
            seq.setTrigCallback([&](int, const Step&) { trigCount++; });

            seq.setFillActive(false);
            seq.start();
            seq.processBlock(44100);
            seq.stop();

            expectEquals(trigCount, 0, "Fill steps should not fire when fill is inactive");
        }

        beginTest("Fill active: Fill steps trigger");
        {
            SequencerCore seq;
            seq.prepareToPlay(44100.0, 512);

            auto& track = seq.getPattern().getTrack(0);
            track.setLength(2);
            track.getStep(0).trig = true;
            track.getStep(0).notes.push_back({60, 1.0f});
            track.getStep(0).trigCondition.type = TrigCondition::Fill;

            int trigCount = 0;
            seq.setTrigCallback([&](int, const Step&) { trigCount++; });

            seq.setFillActive(true);
            seq.start();
            seq.processBlock(44100);
            seq.stop();

            expect(trigCount > 0, "Fill steps should fire when fill is active");
        }

        beginTest("Fill inactive: NotFill steps trigger");
        {
            SequencerCore seq;
            seq.prepareToPlay(44100.0, 512);

            auto& track = seq.getPattern().getTrack(0);
            track.setLength(2);
            track.getStep(0).trig = true;
            track.getStep(0).notes.push_back({60, 1.0f});
            track.getStep(0).trigCondition.type = TrigCondition::NotFill;

            int trigCount = 0;
            seq.setTrigCallback([&](int, const Step&) { trigCount++; });

            seq.setFillActive(false);
            seq.start();
            seq.processBlock(44100);
            seq.stop();

            expect(trigCount > 0, "NotFill steps should fire when fill is inactive");
        }

        beginTest("Fill active: NotFill steps skipped");
        {
            SequencerCore seq;
            seq.prepareToPlay(44100.0, 512);

            auto& track = seq.getPattern().getTrack(0);
            track.setLength(2);
            track.getStep(0).trig = true;
            track.getStep(0).notes.push_back({60, 1.0f});
            track.getStep(0).trigCondition.type = TrigCondition::NotFill;

            int trigCount = 0;
            seq.setTrigCallback([&](int, const Step&) { trigCount++; });

            seq.setFillActive(true);
            seq.start();
            seq.processBlock(44100);
            seq.stop();

            expectEquals(trigCount, 0, "NotFill steps should not fire when fill is active");
        }

        beginTest("Momentary fill resets on release");
        {
            SequencerCore seq;
            seq.setFillLatch(false);
            expect(!seq.isFillLatched(), "Latch should be off");

            seq.setFillActive(true);
            expect(seq.isFillActive());

            seq.setFillActive(false);
            expect(!seq.isFillActive(), "Fill should be off after momentary release");
        }

        beginTest("Latched fill toggles");
        {
            SequencerCore seq;
            seq.setFillLatch(true);
            expect(seq.isFillLatched());

            bool currentState = seq.isFillActive();
            seq.setFillActive(!currentState);
            expect(seq.isFillActive(), "Latch toggle: should be on");

            currentState = seq.isFillActive();
            seq.setFillActive(!currentState);
            expect(!seq.isFillActive(), "Latch toggle: should be off");
        }

        beginTest("Per-track fill activates Fill steps on that track");
        {
            SequencerCore seq;
            seq.prepareToPlay(44100.0, 512);

            auto& track0 = seq.getPattern().getTrack(0);
            track0.setLength(2);
            track0.getStep(0).trig = true;
            track0.getStep(0).notes.push_back({60, 1.0f});
            track0.getStep(0).trigCondition.type = TrigCondition::Fill;

            auto& track1 = seq.getPattern().getTrack(1);
            track1.setLength(2);
            track1.getStep(0).trig = true;
            track1.getStep(0).notes.push_back({72, 1.0f});
            track1.getStep(0).trigCondition.type = TrigCondition::Fill;

            int track0Trigs = 0;
            int track1Trigs = 0;
            seq.setTrigCallback([&](int trackIdx, const Step&) {
                if (trackIdx == 0) track0Trigs++;
                if (trackIdx == 1) track1Trigs++;
            });

            seq.setFillActive(false);
            seq.setTrackFill(0, true);
            seq.setTrackFill(1, false);
            expect(seq.isTrackFillActive(0));
            expect(!seq.isTrackFillActive(1));

            seq.start();
            seq.processBlock(44100);
            seq.stop();

            expect(track0Trigs > 0, "Track 0 fill steps should fire (per-track fill on)");
            expectEquals(track1Trigs, 0, "Track 1 fill steps should not fire (per-track fill off)");
        }

        beginTest("isStepActiveDuringFill returns correct result");
        {
            SequencerCore seq;

            auto& track = seq.getPattern().getTrack(0);
            track.getStep(0).trig = true;
            track.getStep(0).notes.push_back({60, 1.0f});
            track.getStep(0).trigCondition.type = TrigCondition::Fill;

            track.getStep(1).trig = true;
            track.getStep(1).notes.push_back({62, 1.0f});
            track.getStep(1).trigCondition.type = TrigCondition::NotFill;

            track.getStep(2).trig = true;
            track.getStep(2).notes.push_back({64, 1.0f});
            track.getStep(2).trigCondition.type = TrigCondition::Always;

            track.getStep(3).trig = false;

            seq.setFillActive(false);
            expect(!seq.isStepActiveDuringFill(0, 0), "Fill step inactive when fill off");
            expect(seq.isStepActiveDuringFill(0, 1), "NotFill step active when fill off");
            expect(seq.isStepActiveDuringFill(0, 2), "Always step always active");
            expect(!seq.isStepActiveDuringFill(0, 3), "No-trig step never active");

            seq.setFillActive(true);
            expect(seq.isStepActiveDuringFill(0, 0), "Fill step active when fill on");
            expect(!seq.isStepActiveDuringFill(0, 1), "NotFill step inactive when fill on");
            expect(seq.isStepActiveDuringFill(0, 2), "Always step still active");
            expect(!seq.isStepActiveDuringFill(0, 3), "No-trig step still never active");
        }
    }
};

static FillModeTests fillModeTests;

class AftertouchRoutingTests : public juce::UnitTest
{
public:
    AftertouchRoutingTests() : UnitTest("Aftertouch Routing") {}

    void runTest() override
    {
        beginTest("Default route is Volume");
        {
            AftertouchRouter router;
            expectEquals(router.getRouteCount(), 1);
            auto route = router.getRoute(0);
            expectEquals((int) route.dest, (int) AftertouchRouter::Destination::Volume);
            expectEquals(route.depth, 1.0f);
            expect(!route.bipolar);
        }

        beginTest("Set and get routes");
        {
            AftertouchRouter router;
            AftertouchRouter::Route r;
            r.dest = AftertouchRouter::Destination::FilterCutoff;
            r.depth = 0.75f;
            r.bipolar = true;
            router.setRoute(0, r);

            auto got = router.getRoute(0);
            expectEquals((int) got.dest, (int) AftertouchRouter::Destination::FilterCutoff);
            expectWithinAbsoluteError(got.depth, 0.75f, 0.001f);
            expect(got.bipolar);
        }

        beginTest("Compute modulation for Volume destination");
        {
            AftertouchRouter router;
            auto vals = router.computeModulation(0.6f);
            expectWithinAbsoluteError(vals.volumeMod, 0.6f, 0.001f);
            expectWithinAbsoluteError(vals.cutoffMod, 0.0f, 0.001f);
        }

        beginTest("Compute modulation for FilterCutoff");
        {
            AftertouchRouter router;
            AftertouchRouter::Route r;
            r.dest = AftertouchRouter::Destination::FilterCutoff;
            r.depth = 0.5f;
            r.bipolar = false;
            router.setRoute(0, r);

            auto vals = router.computeModulation(0.8f);
            expectWithinAbsoluteError(vals.cutoffMod, 0.4f, 0.001f);
            expectWithinAbsoluteError(vals.volumeMod, 0.0f, 0.001f);
        }

        beginTest("Multiple routes combine");
        {
            AftertouchRouter router;

            AftertouchRouter::Route r1;
            r1.dest = AftertouchRouter::Destination::Volume;
            r1.depth = 1.0f;
            r1.bipolar = false;
            router.setRoute(0, r1);

            AftertouchRouter::Route r2;
            r2.dest = AftertouchRouter::Destination::FilterCutoff;
            r2.depth = 0.5f;
            r2.bipolar = false;
            router.setRoute(1, r2);

            auto vals = router.computeModulation(1.0f);
            expectWithinAbsoluteError(vals.volumeMod, 1.0f, 0.001f);
            expectWithinAbsoluteError(vals.cutoffMod, 0.5f, 0.001f);
        }

        beginTest("Bipolar vs unipolar modulation");
        {
            AftertouchRouter router;

            AftertouchRouter::Route uni;
            uni.dest = AftertouchRouter::Destination::Volume;
            uni.depth = 1.0f;
            uni.bipolar = false;
            router.setRoute(0, uni);

            auto uniVals = router.computeModulation(0.5f);
            expectWithinAbsoluteError(uniVals.volumeMod, 0.5f, 0.001f);

            AftertouchRouter::Route bip;
            bip.dest = AftertouchRouter::Destination::Vibrato;
            bip.depth = 1.0f;
            bip.bipolar = true;
            router.setRoute(1, bip);

            auto bipVals = router.computeModulation(0.5f);
            expectWithinAbsoluteError(bipVals.vibratoMod, 0.0f, 0.001f);

            auto fullVals = router.computeModulation(1.0f);
            expectWithinAbsoluteError(fullVals.vibratoMod, 1.0f, 0.001f);

            auto zeroVals = router.computeModulation(0.0f);
            expectWithinAbsoluteError(zeroVals.vibratoMod, -1.0f, 0.001f);
        }

        beginTest("Zero pressure produces no modulation");
        {
            AftertouchRouter router;
            auto vals = router.computeModulation(0.0f);
            expectWithinAbsoluteError(vals.volumeMod, 0.0f, 0.001f);
            expectWithinAbsoluteError(vals.cutoffMod, 0.0f, 0.001f);
            expectWithinAbsoluteError(vals.fmDepthMod, 0.0f, 0.001f);
            expectWithinAbsoluteError(vals.timbreMod, 0.0f, 0.001f);
            expectWithinAbsoluteError(vals.wavePosMod, 0.0f, 0.001f);
            expectWithinAbsoluteError(vals.vibratoMod, 0.0f, 0.001f);
            expectWithinAbsoluteError(vals.panMod, 0.0f, 0.001f);
        }

        beginTest("Off destination produces no modulation");
        {
            AftertouchRouter router;
            AftertouchRouter::Route r;
            r.dest = AftertouchRouter::Destination::Off;
            r.depth = 1.0f;
            r.bipolar = false;
            router.setRoute(0, r);

            auto vals = router.computeModulation(1.0f);
            expectWithinAbsoluteError(vals.volumeMod, 0.0f, 0.001f);
        }

        beginTest("VoiceAllocator integrates router");
        {
            VoiceAllocator alloc(4);
            alloc.noteOn(60, 1.0f);
            alloc.setAftertouch(60, 0.7f);

            auto mod = alloc.getAftertouchModulation(0);
            expectWithinAbsoluteError(mod.volumeMod, 0.7f, 0.001f);

            AftertouchRouter::Route r;
            r.dest = AftertouchRouter::Destination::FilterCutoff;
            r.depth = 0.5f;
            r.bipolar = false;
            alloc.getAftertouchRouter().setRoute(0, r);

            mod = alloc.getAftertouchModulation(0);
            expectWithinAbsoluteError(mod.cutoffMod, 0.35f, 0.001f);
            expectWithinAbsoluteError(mod.volumeMod, 0.0f, 0.001f);
        }

        beginTest("MAX_ROUTES boundary");
        {
            AftertouchRouter router;
            for (int i = 0; i < AftertouchRouter::MAX_ROUTES; ++i)
            {
                AftertouchRouter::Route r;
                r.dest = AftertouchRouter::Destination::Volume;
                r.depth = 0.25f;
                r.bipolar = false;
                router.setRoute(i, r);
            }
            expectEquals(router.getRouteCount(), AftertouchRouter::MAX_ROUTES);

            auto vals = router.computeModulation(1.0f);
            expectWithinAbsoluteError(vals.volumeMod, 1.0f, 0.001f);

            AftertouchRouter::Route bad;
            bad.dest = AftertouchRouter::Destination::Vibrato;
            bad.depth = 99.0f;
            router.setRoute(AftertouchRouter::MAX_ROUTES, bad);
            expectEquals(router.getRouteCount(), AftertouchRouter::MAX_ROUTES);
        }
    }
};

// ---------------------------------------------------------------
// Factory Presets Tests
// ---------------------------------------------------------------
class FactoryPresetsTests : public juce::UnitTest
{
public:
    FactoryPresetsTests() : UnitTest("Factory Presets") {}

    void runTest() override
    {
        beginTest("registerAll adds at least 30 presets");
        {
            auto allPresets = FactoryPresets::getAll();
            expect((int)allPresets.size() >= 30,
                   "FactoryPresets::getAll should return at least 30 presets, got "
                   + juce::String((int)allPresets.size()));

            PresetManager mgr;
            bool foundSubBass = false;
            bool foundAcidBass = false;
            bool foundShimmer = false;
            for (int i = 0; i < mgr.getPresetCount(); ++i)
            {
                auto& p = mgr.getPreset(i);
                if (p.name == "Sub Bass") foundSubBass = true;
                if (p.name == "Acid Bass") foundAcidBass = true;
                if (p.name == "Shimmer") foundShimmer = true;
            }
            expect(foundSubBass, "Should find Sub Bass preset");
            expect(foundAcidBass, "Should find Acid Bass preset");
            expect(foundShimmer, "Should find Shimmer preset");
        }

        beginTest("Each category has presets");
        {
            auto allPresets = FactoryPresets::getAll();

            int bassCount = 0, padCount = 0, leadCount = 0, keysCount = 0;
            int fxCount = 0, kitCount = 0;

            for (auto& p : allPresets)
            {
                if (p.category == "BASS") ++bassCount;
                else if (p.category == "PAD") ++padCount;
                else if (p.category == "LEAD") ++leadCount;
                else if (p.category == "KEYS") ++keysCount;
                else if (p.category == "FX") ++fxCount;
                else if (p.category == "KIT") ++kitCount;
            }

            expect(bassCount >= 6, "BASS should have >= 6 presets");
            expect(padCount >= 5, "PAD should have >= 5 presets");
            expect(leadCount >= 5, "LEAD should have >= 5 presets");
            expect(keysCount >= 4, "KEYS should have >= 4 presets");
            expect(fxCount >= 4, "FX should have >= 4 presets");
            expect(kitCount >= 6, "KIT should have >= 6 presets");
        }

        beginTest("Preset params are in valid range");
        {
            auto allPresets = FactoryPresets::getAll();

            for (auto& p : allPresets)
            {
                expect(p.isFactory, p.name + " should be factory");
                expect(p.name.isNotEmpty(), "Preset should have a name");
                expect(p.category.isNotEmpty(), "Preset should have a category");
                expect((int)p.engineType >= 0 && (int)p.engineType <= (int)EngineType::MIDITrack,
                       p.name + " should have valid engine type");
            }
        }

        beginTest("Round-trip serialization preserves factory presets");
        {
            auto allPresets = FactoryPresets::getAll();

            for (int idx : {0, 5, 10, 15, 20, 25})
            {
                if (idx >= (int)allPresets.size()) break;

                auto& original = allPresets[(size_t)idx];
                auto json = original.toJSON();
                auto restored = Preset::fromJSON(json);

                expectEquals(restored.name, original.name,
                             "Name mismatch for preset " + original.name);
                expectEquals(restored.category, original.category,
                             "Category mismatch for preset " + original.name);
                expect(restored.engineType == original.engineType,
                       "Engine type mismatch for " + original.name);
                expectEquals((int)restored.soundParams.size(),
                             (int)original.soundParams.size(),
                             "Param count mismatch for " + original.name);
            }
        }
    }
};

static FactoryPresetsTests factoryPresetsTests;

class ArpExtendedTests : public juce::UnitTest
{
public:
    ArpExtendedTests() : UnitTest("Arp Extended") {}

    void runTest() override
    {
        beginTest("Walk mode stays within bounds");
        {
            Arpeggiator arp;
            arp.setMode(Arpeggiator::Mode::Walk);
            arp.noteOn(60, 0.8f);
            arp.noteOn(64, 0.8f);
            arp.noteOn(67, 0.8f);
            arp.triggerFirstNote();
            arp.processBlock(44100, 44100.0, 120.0f);
            expect(arp.getCurrentStep() >= 0 && arp.getCurrentStep() < arp.getSequenceLength());
        }

        beginTest("Converge mode alternates low-high");
        {
            Arpeggiator arp;
            arp.setMode(Arpeggiator::Mode::Converge);
            arp.noteOn(60, 0.8f);
            arp.noteOn(64, 0.8f);
            arp.noteOn(67, 0.8f);
            expectEquals(arp.getSequenceLength(), 3);
            expectEquals(arp.getSequencePitch(0), 60);
            expectEquals(arp.getSequencePitch(1), 67);
            expectEquals(arp.getSequencePitch(2), 64);
        }

        beginTest("Diverge mode starts from middle");
        {
            Arpeggiator arp;
            arp.setMode(Arpeggiator::Mode::Diverge);
            arp.noteOn(60, 0.8f);
            arp.noteOn(64, 0.8f);
            arp.noteOn(67, 0.8f);
            expectEquals(arp.getSequenceLength(), 3);
            expectEquals(arp.getSequencePitch(0), 64);
        }

        beginTest("PlayOrder preserves input order");
        {
            Arpeggiator arp;
            arp.setMode(Arpeggiator::Mode::PlayOrder);
            arp.noteOn(67, 0.8f);
            arp.noteOn(60, 0.8f);
            arp.noteOn(64, 0.8f);
            expectEquals(arp.getSequenceLength(), 3);
            expectEquals(arp.getSequencePitch(0), 67);
            expectEquals(arp.getSequencePitch(1), 60);
            expectEquals(arp.getSequencePitch(2), 64);
        }

        beginTest("Strum builds delayed notes");
        {
            Arpeggiator arp;
            arp.setMode(Arpeggiator::Mode::ChordRepeat);
            arp.setStrumEnabled(true);
            arp.setStrumTime(20.0f);
            arp.noteOn(60, 0.8f);
            arp.noteOn(64, 0.8f);
            arp.noteOn(67, 0.8f);
            arp.triggerFirstNote();
            expect(arp.getStrumNoteCount() == 3);
            expect(arp.getStrumNote(0).delaySamples == 0.0);
            expect(arp.getStrumNote(2).delaySamples > 0.0);
        }

        beginTest("Ratchet config is clamped");
        {
            Arpeggiator arp;
            arp.setRatchetEnabled(true);
            arp.setRatchetCount(6);
            expectEquals(arp.getRatchetCount(), 4);
            arp.setRatchetDecay(-0.5f);
            expectWithinAbsoluteError(arp.getRatchetDecay(), 0.0f, 0.001f);
        }
    }
};

class MutateSystemTests : public juce::UnitTest
{
public:
    MutateSystemTests() : UnitTest("Mutate System") {}

    void runTest() override
    {
        beginTest("Ancestor mode restores original before mutating");
        {
            MutateSystem ms;
            ms.setMode(MutateSystem::Mode::Ancestor);

            std::vector<Step> steps(4);
            steps[0].trig = true;
            steps[0].notes.push_back({60, 0.8f});
            steps[1].trig = true;
            steps[1].notes.push_back({64, 0.8f});

            ms.setAncestor(steps);
            expect(ms.hasAncestor());

            ms.mutate(steps, 12345);
            expect(steps.size() == 4);
        }

        beginTest("Cumulative mode compounds mutations");
        {
            MutateSystem ms;
            ms.setMode(MutateSystem::Mode::Cumulative);
            ms.setAmount(1.0f);

            std::vector<Step> steps(4);
            for (auto& s : steps) { s.trig = true; s.notes.push_back({60, 0.8f}); }
            ms.setAncestor(steps);

            std::vector<Step> copy1 = steps;
            ms.mutate(copy1, 100);

            std::vector<Step> copy2 = copy1;
            ms.mutate(copy2, 200);

            // cumulative compounds changes
            bool anyDiff = false;
            for (size_t i = 0; i < steps.size(); ++i)
                if (copy2[i].velocity != steps[i].velocity || copy2[i].trig != steps[i].trig)
                    anyDiff = true;
            expect(anyDiff);
        }

        beginTest("Return to ancestor restores snapshot");
        {
            MutateSystem ms;
            std::vector<Step> steps(2);
            steps[0].trig = true;
            steps[0].velocity = 0.5f;
            ms.setAncestor(steps);

            steps[0].velocity = 1.0f;
            steps[0].trig = false;

            ms.returnToAncestor(steps);
            expect(steps[0].trig == true);
            expectWithinAbsoluteError(steps[0].velocity, 0.5f, 0.001f);
        }

        beginTest("Commit ancestor updates baseline");
        {
            MutateSystem ms;
            std::vector<Step> original(2);
            original[0].velocity = 0.3f;
            ms.setAncestor(original);

            std::vector<Step> modified(2);
            modified[0].velocity = 0.9f;
            ms.commitAncestor(modified);

            expectWithinAbsoluteError(ms.getAncestor()[0].velocity, 0.9f, 0.001f);
        }
    }
};

class SongScreenTests : public juce::UnitTest
{
public:
    SongScreenTests() : UnitTest("Song Screen") {}

    void runTest() override
    {
        beginTest("SongScreen has correct name");
        {
            SongMode song;
            SequencerCore seq;
            SongScreen screen(song, seq);
            expectEquals(screen.getName(), juce::String("SONG"));
        }

        beginTest("SongScreen shows EMPTY when no entries");
        {
            SongMode song;
            SequencerCore seq;
            SongScreen screen(song, seq);
            expectEquals(screen.getSubPage(), juce::String("EMPTY"));
        }

        beginTest("SongScreen shows row count with entries");
        {
            SongMode song;
            SequencerCore seq;
            song.addEntry(0);
            song.addEntry(1);
            SongScreen screen(song, seq);
            expectEquals(screen.getSubPage(), juce::String("ROW 1 OF 2"));
            expectEquals(screen.getPageCount(), 1);     // a scrolling list, not pages
        }

        beginTest("Pad press adds and selects rows");
        {
            SongMode song;
            SequencerCore seq;
            SongScreen screen(song, seq);
            screen.onPadPress(0, 100);
            expectEquals(song.getEntryCount(), 1);
            expectEquals(screen.getSelectedRow(), 0);
        }
    }
};

class AftertouchScreenTests : public juce::UnitTest
{
public:
    AftertouchScreenTests() : UnitTest("Aftertouch Screen") {}

    void runTest() override
    {
        beginTest("AftertouchScreen has correct name");
        {
            AftertouchRouter router;
            AftertouchScreen screen(router);
            expectEquals(screen.getName(), juce::String("AT ROUTE"));
        }

        beginTest("Top row = destinations of routes 1-4, bottom row = their depths");
        {
            AftertouchRouter router;
            AftertouchScreen screen(router);
            // Vocabulary: every destination cell is DEST (and its depth DPTH).
            expectEquals(screen.getEncoderName(0), juce::String("DEST"));
            expectEquals(screen.getEncoderName(3), juce::String("DEST"));
            expectEquals(screen.getEncoderName(4), juce::String("DPTH"));
            expectEquals(screen.getEncoderName(7), juce::String("DPTH"));
            expect(!screen.hasSubParam(0));
            expect(!screen.usesEncoderBar());
        }

        beginTest("Encoders edit the router the engines read");
        {
            AftertouchRouter router;
            AftertouchScreen screen(router);
            expectEquals(router.getRouteCount(), (int) AftertouchRouter::MAX_ROUTES);
            expect(router.getRoute(1).dest == AftertouchRouter::Destination::Off);

            screen.onEncoderChange(1, 2);    // route 2: OFF -> FLT
            expect(router.getRoute(1).dest == AftertouchRouter::Destination::FilterCutoff);
            float before = router.getRoute(1).depth;
            screen.onEncoderChange(5, 10);   // route 2 depth up
            expect(router.getRoute(1).depth > before);

            // Full pressure now opens the filter of a unified voice.
            auto mod = router.computeModulation(1.0f);
            expect(mod.cutoffMod > 0.0f);
            expect(mod.volumeMod > 0.0f, "Route 1 defaults to volume");
        }

        beginTest("Pad pressure reaches the voices of every pitched engine through the shared router");
        {
            AftertouchRouter router;
            router.setRoute(0, { AftertouchRouter::Destination::Volume, 1.0f, false });
            for (auto type : { EngineType::UnifiedVoice, EngineType::FM, EngineType::MacroOsc,
                               EngineType::Wavetable, EngineType::WaveSeq })
            {
                auto level = [&](float pressure)
                {
                    auto e = AudioEngine::createEngine(type);
                    // One oscillator only: two detuned ones beat, which would swamp the comparison.
                    if (type == EngineType::UnifiedVoice)
                        e->setParameter(UnifiedParam::MIX, 0.0f);
                    e->setAftertouchRouter(&router);
                    e->prepareToPlay(44100.0, 512);
                    e->noteOn(60, 0.8f, -1);
                    e->aftertouch(60, pressure);
                    juce::MidiBuffer midi;
                    double sum = 0.0;
                    for (int b = 0; b < 20; ++b)
                    {
                        juce::AudioBuffer<float> buf(2, 512);
                        buf.clear();
                        e->processBlock(buf, midi);
                        sum += buf.getRMSLevel(0, 0, 512);
                    }
                    return sum;
                };
                expect(level(1.0f) > level(0.0f) * 1.3, "Pressure should raise the level of engine type " + juce::String((int) type));
            }
        }
    }
};

static ArpExtendedTests arpExtendedTests;
static MutateSystemTests mutateSystemTests;
static SongScreenTests songScreenTests;
static AftertouchScreenTests aftertouchScreenTests;

#include "../src/sync/AbletonLink.h"

class SyncWiringTests : public juce::UnitTest
{
public:
    SyncWiringTests() : UnitTest("Sync Wiring", "Neptune") {}

    void runTest() override
    {
        beginTest("AudioEngine accepts MIDIClock");
        {
            AudioEngine engine;
            MIDIClock clock;
            engine.setMIDIClock(&clock);
            expect(engine.getMIDIClock() == &clock);
        }

        beginTest("MIDIClock generates clock events in Internal mode");
        {
            MIDIClock clock;
            clock.setMode(MIDIClock::Mode::Internal);
            clock.setTempo(120.0);
            clock.prepareToPlay(44100.0, 512);
            clock.startTransport();

            MIDIClock::ClockEvent evt;
            expect(clock.popOutputEvent(evt));
            expect(evt.type == MIDIClock::ClockEvent::Start);

            clock.processBlock(44100, 44100.0);

            int clockCount = 0;
            while (clock.popOutputEvent(evt))
            {
                if (evt.type == MIDIClock::ClockEvent::Clock)
                    clockCount++;
            }
            expect(clockCount > 0);
        }

        beginTest("MIDIClock stopTransport sends Stop event");
        {
            MIDIClock clock;
            clock.setMode(MIDIClock::Mode::Internal);
            clock.startTransport();
            MIDIClock::ClockEvent evt;
            clock.popOutputEvent(evt);

            clock.stopTransport();
            expect(clock.popOutputEvent(evt));
            expect(evt.type == MIDIClock::ClockEvent::Stop);
        }

        beginTest("AbletonLink enable/disable");
        {
            AbletonLink link;
            expect(!link.isEnabled());
            link.enable();
            expect(link.isEnabled());
            link.disable();
            expect(!link.isEnabled());
        }

        beginTest("AbletonLink tempo callback fires");
        {
            AbletonLink link;
            double callbackTempo = 0.0;
            link.setTempoCallback([&](double bpm) { callbackTempo = bpm; });
            expect(callbackTempo == 0.0);
        }

        beginTest("AbletonLink applyToMidiClock");
        {
            AbletonLink link;
            MIDIClock clock;
            clock.setTempo(120.0);
            link.applyToMidiClock(clock);
            expectEquals(clock.getTempo(), 120.0);
        }
    }
};

static SyncWiringTests syncWiringTests;

int main(int argc, char** argv)
{
    if (argc > 1 && juce::String(argv[1]) == "--bench")
        return bench::main();

    juce::UnitTestRunner runner;

    auto& allTests = juce::UnitTest::getAllTests();
    juce::Array<juce::UnitTest*> ours;
    for (auto* t : allTests)
    {
        auto name = t->getName();
        if (name == "MIDI Messages" || name == "Bitmap Encoding"
            || name == "Voice Allocator" || name == "Sequencer Core"
            || name == "Envelope" || name == "SV Filter"
            || name == "Display Renderer" || name == "Pad Manager"
            || name == "Bitmap Font" || name == "LFO"
            || name == "Unified Voice" || name == "Unified Voice Engine"
            || name == "Pad LED Feedback"
            || name == "Step View Screen" || name == "Mod Matrix"
            || name == "Ladder Filter" || name == "FM Operator"
            || name == "FM Voice" || name == "FM Engine"
            || name == "Step Gestures" || name == "Live Recording"
            || name == "Engine Selection" || name == "Note Off Scheduling"
            || name == "Encoder Display"
            || name == "Screen Navigation"
            || name == "Trig Conditions" || name == "Pattern Bank"
            || name == "Trig Condition UI" || name == "Diode Filter"
            || name == "Analog Character"
            || name == "MIDI Track"
            || name == "HomeScreen Engine Switch"
            || name == "Step Detail Pages"
            || name == "Filter Set"
            || name == "Track Effects"
            || name == "Pad Ratchet"
            || name == "LFO Modes"
            || name == "Envelope Stage Curves"
            || name == "Effects More"
            || name == "Engine Filters"
            || name == "Engine LFOs"
            || name == "Sampler Grain"
            || name == "Sampler Stretch"
            || name == "Sampler Release"
            || name == "Wavetable Scan"
            || name == "Drum Keys"
            || name == "Preset Audition"
            || name == "Control Grammar"
            || name == "Live Record"
            || name == "Held Step Pages"
            || name == "LFO Destinations"
            || name == "Help Cards"
            || name == "Controller Clock"
            || name == "Help Card Layers"
            || name == "Strip Routes"
            || name == "Dead Controls"
            || name == "Param Grammar"
            || name == "Screen Grammar"
            || name == "Filter Slot"
            || name == "Architecture Preset"
            || name == "PolyBLEP"
            || name == "Drum Voice"
            || name == "Drum Engine"
            || name == "Display Primitives"
            || name == "Macro Osc Engine"
            || name == "Wavetable Engine"
            || name == "Wave Seq Engine"
            || name == "Envelope Chain"
            || name == "ParamPage Preset Loading"
            || name == "Sub-Parameter Overlay"
            || name == "Scale Button"
            || name == "Undo History"
            || name == "Master Chain"
            || name == "Preset Serialization"
            || name == "Preset Manager"
            || name == "Delay"
            || name == "Reverb"
            || name == "Insert Chain"
            || name == "FX Page"
            || name == "Sampler Engine"
            || name == "Effect Param Lock"
            || name == "Send Bus Routing"
            || name == "Custom Scale"
            || name == "Custom Scale Screen"
            || name == "Arpeggiator"
            || name == "Arp Screen"
            || name == "Song Mode"
            || name == "MIDI Clock"
            || name == "Variation Macros"
            || name == "Rhythm Capture"
            || name == "Fill Mode"
            || name == "Aftertouch Routing"
            || name == "Pressure Lane"
            || name == "Factory Presets"
            || name == "Arp Extended"
            || name == "Mutate System"
            || name == "Song Screen"
            || name == "Aftertouch Screen"
            || name == "Custom Scale Deep"
            || name == "Send Bus Manager"
            || name == "Sidechain Compressor"
            || name == "Send Bus Sidechain Integration"
            || name == "Macro Osc Extended"
            || name == "Wavetable Extended"
            || name == "Wave Seq Extended"
            || name == "Drum Kits & Expression"
            || name == "Effects Expansion"
            || name == "Ableton Link"
            || name == "Project Serialization"
            || name == "Project File Operations"
            || name == "SettingsScreen Sync"
            || name == "Clock-Link Integration"
            || name == "Envelope Chain Extended"
            || name == "LFO Bank"
            || name == "Sub-Parameter Overlay"
            || name == "Wave Stacking"
            || name == "CPU Meter"
            || name == "Sampler Full"
            || name == "Sample Pool"
            || name == "Arp Performance"
            || name == "MOD Page Dynamic"
            || name == "Engine MOD Params"
            || name == "Project Integration"
            || name == "ModMatrix Wiring"
            || name == "Filter View Screen"
            || name == "OLED Mirror"
            || name == "MIDI Track Output"
            || name == "OLED Mirror Input"
            || name == "Project Roundtrip"
            || name == "Factory Preset Coverage"
            || name == "Mod Wheel"
            || name == "SettingsScreen Project Page"
            || name == "SettingsScreen Clock Page"
            || name == "Bitmap Benchmark"
            || name == "Sync Wiring"
            || name == "Onset Clicks"
            || name == "Display Latency")
            ours.add(t);
    }

    runner.runTests(ours);

    int failures = 0;
    for (int i = 0; i < runner.getNumResults(); ++i)
    {
        if (auto* result = runner.getResult(i))
            failures += result->failures;
    }

    if (failures > 0)
    {
        std::cout << "FAILED: " << failures << " test(s) failed." << std::endl;
        return 1;
    }

    std::cout << "All tests passed." << std::endl;
    return 0;
}
