cmake_minimum_required(VERSION 3.22)
project(NeptuneGroovebox VERSION 0.9.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

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
    src/dsp/ModMatrix.h
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
    src/dsp/effects/EffectChain.h
    src/dsp/effects/SendBus.h
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

enable_testing()

juce_add_console_app(NeptuneTests
    PRODUCT_NAME "Neptune Tests"
)

target_sources(NeptuneTests PRIVATE
    tests/main_test.cpp
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

# The drum breaks, when assets/breaks holds any (see the top of this file).
if(TARGET NeptuneBreakData)
    target_link_libraries(NeptuneGroovebox PRIVATE NeptuneBreakData)
    target_link_libraries(NeptuneTests PRIVATE NeptuneBreakData)
endif()

add_test(NAME NeptuneTests COMMAND NeptuneTests_artefacts/Debug/NeptuneTests)
