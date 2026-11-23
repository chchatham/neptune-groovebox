cmake_minimum_required(VERSION 3.22)
project(NeptuneGroovebox VERSION 0.9.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ccache, where it is installed: the worktrees of parallel agents then share compiled objects (JUCE's
# above all). base_dir makes paths inside a checkout relative, so two checkouts give the same key.
find_program(NEPTUNE_CCACHE ccache)
if(NEPTUNE_CCACHE)
    foreach(lang C CXX OBJC OBJCXX)
        set(CMAKE_${lang}_COMPILER_LAUNCHER "${NEPTUNE_CCACHE}" "base_dir=${CMAKE_SOURCE_DIR}" "hash_dir=false")
    endforeach()
endif()

include(FetchContent)

FetchContent_Declare(
    JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG        9.0.5
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(JUCE)

# The drum breaks of assets/breaks (FLAC; credits in docs/sample-credits.md) are built into the app
# and into the tests, so both read the same bytes and nothing has to be found at run time.
# SampleBank.h includes NeptuneBreakData.h when it exists; without the files the build still works
# and the bank holds only the breaks it renders itself.
file(GLOB NEPTUNE_BREAK_FILES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/assets/breaks/*.flac")
if(NEPTUNE_BREAK_FILES)
    juce_add_binary_data(NeptuneBreakData
        HEADER_NAME "NeptuneBreakData.h"
        NAMESPACE NeptuneBreaks
        SOURCES ${NEPTUNE_BREAK_FILES})
endif()

juce_add_gui_app(NeptuneGroovebox
    PRODUCT_NAME "Neptune Groovebox"
    COMPANY_NAME "Neptune"
    BUNDLE_ID "com.neptune.groovebox"
)

target_sources(NeptuneGroovebox PRIVATE
    src/main.cpp
    src/controller/LaunchkeyController.h
    src/controller/LaunchkeyController.cpp
    src/controller/DisplayRenderer.h
    src/controller/DisplayRenderer.cpp
    src/controller/ControllerProfile.h
    src/controller/PadManager.h
    src/controller/PadManager.cpp
    src/ui/BitmapFont.h
    src/ui/ScreenManager.h
    src/ui/ScreenManager.cpp
    src/ui/screens/HomeScreen.h
    src/ui/screens/MixerScreen.h
    src/ui/screens/SettingsScreen.h
    src/ui/screens/StepViewScreen.h
    src/ui/screens/ParamPageScreen.h
    src/ui/PadLEDFeedback.h
    src/engine/AudioEngine.h
    src/engine/EngineBase.h
    src/engine/AftertouchRouter.h
    src/engine/VoiceAllocator.h
    src/engine/VoiceAllocator.cpp
    src/engine/UnifiedVoice.h
    src/engine/UnifiedVoiceEngine.h
    src/engine/FMOperator.h
    src/engine/FMVoice.h
    src/engine/FMEngine.h
    src/engine/MacroOscEngine.h
    src/engine/WavetableEngine.h
    src/dsp/Wavetable.h
    src/engine/VoiceFilterSection.h
    src/engine/VoiceSpread.h
    src/engine/WaveSeqEngine.h
    src/engine/MIDITrack.h
    src/engine/ArchitecturePreset.h
    src/engine/DrumVoice.h
    src/engine/DrumEngine.h
    src/engine/SamplerEngine.h
    src/ui/screens/DrumPadScreen.h
    src/ui/Icons.h
    src/ui/ScreenLayout.h
    src/dsp/EnvelopeChain.h
    src/dsp/filters/FilterSlot.h
    src/dsp/filters/DiodeFilter.h
    src/sequencer/Step.h
    src/sequencer/Track.h
    src/sequencer/Pattern.h
    src/sequencer/SequencerCore.h
    src/sequencer/SequencerCore.cpp
    src/dsp/Envelope.h
    src/dsp/LFO.h
    src/dsp/filters/SVFilter.h
    src/dsp/filters/LadderFilter.h
    src/project/UndoHistory.h
    src/project/Preset.h
    src/project/PresetManager.h
    src/ui/screens/PresetScreen.h
    src/ui/screens/WaveformScreen.h
    src/dsp/effects/EffectBase.h
    src/dsp/effects/ParametricEQ.h
    src/dsp/effects/Compressor.h
    src/dsp/effects/Limiter.h
    src/dsp/effects/Detector.h
    src/dsp/effects/EqBand.h
    src/dsp/effects/CellEffect.h
    src/dsp/effects/EffectChain.h
    src/dsp/effects/SendBus.h
    src/dsp/effects/FxLfo.h
    src/dsp/effects/SweptFilter.h
    src/dsp/effects/DirtEffect.h
    src/dsp/effects/TalkEffect.h
    src/dsp/TempoDivisions.h
    src/dsp/effects/ModDelay.h
    src/dsp/effects/Chorus.h
    src/dsp/effects/Drive.h
    src/dsp/effects/Flanger.h
    src/dsp/effects/Phaser.h
    src/dsp/effects/Tremolo.h
    src/dsp/effects/Delay.h
    src/dsp/effects/Reverb.h
    src/controller/CustomScaleManager.h
    src/ui/screens/CustomScaleScreen.h
    src/arpeggiator/Arpeggiator.h
    src/arpeggiator/VariationMacros.h
    src/arpeggiator/RhythmCapture.h
    src/ui/screens/ArpScreen.h
    src/sequencer/SongMode.h
    src/sync/MIDIClock.h
    src/sync/MIDIRouter.h
    src/project/FactoryPresets.h
    src/project/ProjectManager.h
    src/ui/OLEDMirrorComponent.h
    src/ui/VirtualControllerComponent.h
    src/ui/MainContentComponent.h
    src/ui/StartupAnimation.h
    src/ui/screens/FilterViewScreen.h
    src/ui/screens/SongScreen.h
    src/ui/screens/AftertouchScreen.h
    src/ui/screens/SubParamOverlay.h
    src/controller/BitmapBenchmark.h
)

target_include_directories(NeptuneGroovebox PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_definitions(NeptuneGroovebox PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_DISPLAY_SPLASH_SCREEN=0
    JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:NeptuneGroovebox,JUCE_PRODUCT_NAME>"
    JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:NeptuneGroovebox,JUCE_VERSION>"
)

target_link_libraries(NeptuneGroovebox PRIVATE
    juce::juce_audio_basics
    juce::juce_audio_devices
    juce::juce_audio_formats
    juce::juce_audio_processors
    juce::juce_audio_utils
    juce::juce_core
    juce::juce_events
    juce::juce_graphics
    juce::juce_gui_basics
    juce::juce_gui_extra
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags
)

# ---- AU / VST3 plugin target ------------------------------------------------
# Neptune as a DAW instrument plugin. Loads inside Ableton, Logic, etc. and
# receives MIDI + transport while outputting stereo audio.
juce_add_plugin(NeptunePlugin
    PRODUCT_NAME "Neptune Groovebox"
    COMPANY_NAME "Neptune"
    BUNDLE_ID "com.neptune.groovebox.plugin"
    PLUGIN_MANUFACTURER_CODE Nept
    PLUGIN_CODE Ngrb
    FORMATS AU VST3
    IS_SYNTH TRUE
    NEEDS_MIDI_INPUT TRUE
    NEEDS_MIDI_OUTPUT TRUE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    COPY_PLUGIN_AFTER_BUILD TRUE
)

target_sources(NeptunePlugin PRIVATE
    src/plugin/PluginProcessor.h
    src/plugin/PluginProcessor.cpp
    src/engine/VoiceAllocator.cpp
    src/sequencer/SequencerCore.cpp
)

target_include_directories(NeptunePlugin PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_definitions(NeptunePlugin PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_DISPLAY_SPLASH_SCREEN=0
    JUCE_VST3_CAN_REPLACE_VST2=0
)

target_link_libraries(NeptunePlugin PRIVATE
    juce::juce_audio_basics
    juce::juce_audio_devices
    juce::juce_audio_formats
    juce::juce_audio_processors
    juce::juce_audio_utils
    juce::juce_core
    juce::juce_events
    juce::juce_graphics
    juce::juce_gui_basics
    juce::juce_gui_extra
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags
)

if(TARGET NeptuneBreakData)
    target_link_libraries(NeptunePlugin PRIVATE NeptuneBreakData)
endif()

enable_testing()

juce_add_console_app(NeptuneTests
    PRODUCT_NAME "Neptune Tests"
)

target_sources(NeptuneTests PRIVATE
    tests/main_test.cpp
    tests/fast_measure.cpp
    src/engine/VoiceAllocator.cpp
    src/sequencer/SequencerCore.cpp
    src/controller/DisplayRenderer.cpp
    src/controller/LaunchkeyController.cpp
    src/controller/PadManager.cpp
    src/ui/ScreenManager.cpp
)

target_include_directories(NeptuneTests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# The tests' measuring arithmetic (FFTs over what was rendered) is optimised in every build type:
# in the Debug build it was most of the suite's time. The instrument is compiled as the build type says.
set_source_files_properties(tests/fast_measure.cpp PROPERTIES COMPILE_OPTIONS "-O2")

target_link_libraries(NeptuneTests PRIVATE
    juce::juce_core
    juce::juce_audio_basics
    juce::juce_audio_devices
    juce::juce_audio_formats
    juce::juce_events
    juce::juce_graphics
    juce::juce_gui_basics
    juce::juce_recommended_config_flags
)

target_compile_definitions(NeptuneTests PRIVATE
    JUCE_UNIT_TESTS=1
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
)

# Where the tests find their fixtures and the repository's files. Not __FILE__: under ccache's base_dir
# that is a relative path, and a test then passes or fails by the folder it was started in. On the one
# file only, so the JUCE objects of this target stay the same in every checkout (ccache shares them).
set_source_files_properties(tests/main_test.cpp PROPERTIES
    COMPILE_DEFINITIONS "NEPTUNE_TESTS_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/tests\"")

# The drum breaks, when assets/breaks holds any (see the top of this file).
if(TARGET NeptuneBreakData)
    target_link_libraries(NeptuneGroovebox PRIVATE NeptuneBreakData)
    target_link_libraries(NeptuneTests PRIVATE NeptuneBreakData)
endif()

# The suite is one binary, run as several tests so that `ctest -j 8` runs them side by side (about a
# minute; one after the other they take about five). Each slow suite is named in one group; the last
# test runs every suite that no group names, so a new suite runs without being added here. Rules:
#   - a suite is named in one group only (names are matched as parts of suite names, see main() in
#     tests/main_test.cpp), and a group stays under about 30 s (time them with NEPTUNE_TEST_ONLY);
#   - "Mod Wheel" and "Strip Routes" stay together: the second reuses the first one's renders;
#   - only suites that render sound and touch no file, folder or MIDI port may be named: everything
#     that writes projects, presets or the recent list, or opens a port, stays in the last test, where
#     the suites run one after the other.
set(NEPTUNE_TEST_GROUPS
    "Click Audit"
    "Dead Controls"
    "Mod Wheel|Strip Routes"
    "Send Reverb|Engine LFOs"
    "Sampler Stretch|Macro Osc Extended"
    "Preset Audition|Cymbal Machine"
    "LFO Destinations|Wavetable Embed|Sampler Release"
    "Undo History|Effects More|Sampler Grain|Wavetable Keyframes|Wavetable Scan"
    "Perf FX"
    # The unification's suites (2026-11-22: "everything-else" had grown to over five minutes alone)
    "U4 Sampler"
    "U4 FM|U4 Wavetable"
    "U4 Wave Sequencer|Voice Body"
    "Resonator"
    "Effect Cores"
    "Vocoder|XIN"
    "P01 Keyboard|Master Chain|Router Sources"
    "Mod Slots|Voice Filter Spread|Phase Osc|Mod Router"
    "Send Chains"
    "U4 Unified"
    "U4 RSN"
    "U4 Drum|Router Phase"
    "U4 Macro"
    "Effect Destinations"
    # Out of "everything-else" (2026-11-22: it was 125 s alone and is in every merge run; about 40 s without these)
    "One Network|Pinned On Cores"
    "Track Input"
    "Kick Points|Saturation"
    # The preset suites of U6 (93 s together inside "everything-else", which had reached 148 s)
    "Factory Multis|Fusion Presets"
    "Preset Distinctness"
)
set(NEPTUNE_TEST_NAMED "Robustness")
foreach(group IN LISTS NEPTUNE_TEST_GROUPS)
    string(REGEX REPLACE "[ |]+" "-" label "${group}")
    add_test(NAME "NeptuneTests-${label}" COMMAND NeptuneTests)
    set_tests_properties("NeptuneTests-${label}" PROPERTIES ENVIRONMENT "NEPTUNE_TEST_ONLY=${group}")
    string(APPEND NEPTUNE_TEST_NAMED "|${group}")
endforeach()
# "Robustness" in two halves (its parts 1-9: NEPTUNE_ROBUST_ONLY in tests/robustness_tests.h).
add_test(NAME "NeptuneTests-Robustness-1235" COMMAND NeptuneTests)
set_tests_properties("NeptuneTests-Robustness-1235" PROPERTIES
    ENVIRONMENT "NEPTUNE_TEST_ONLY=Robustness;NEPTUNE_ROBUST_ONLY=1235")
add_test(NAME "NeptuneTests-Robustness-46789" COMMAND NeptuneTests)
set_tests_properties("NeptuneTests-Robustness-46789" PROPERTIES
    ENVIRONMENT "NEPTUNE_TEST_ONLY=Robustness;NEPTUNE_ROBUST_ONLY=46789")
add_test(NAME "NeptuneTests-everything-else" COMMAND NeptuneTests)
set_tests_properties("NeptuneTests-everything-else" PROPERTIES ENVIRONMENT "NEPTUNE_TEST_EXCEPT=${NEPTUNE_TEST_NAMED}")
