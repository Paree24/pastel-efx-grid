cmake_minimum_required(VERSION 3.22)
project(PastelEfxGrid VERSION 0.1.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(FetchContent)

# Prefer a local JUCE checkout (fast, offline). Override with -DPASTEL_JUCE_DIR=...
set(PASTEL_JUCE_DIR "/home/paree/ai_experiments/Nanoberg/JUCE" CACHE PATH "Local JUCE source tree (optional)")
if(EXISTS "${PASTEL_JUCE_DIR}/CMakeLists.txt")
    message(STATUS "Using local JUCE at ${PASTEL_JUCE_DIR}")
    FetchContent_Declare(JUCE SOURCE_DIR "${PASTEL_JUCE_DIR}")
else()
    message(STATUS "Fetching JUCE from git")
    FetchContent_Declare(
        JUCE
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG 8.0.6
        GIT_SHALLOW TRUE
    )
endif()
FetchContent_MakeAvailable(JUCE)

juce_add_plugin(PastelEfxGrid
    COMPANY_NAME "Pastel EFX"
    BUNDLE_ID "com.pastelefx.efxgrid"
    DESCRIPTION "Pastel EFX Grid - a 4x4 grid of effect slots, any of 91 effects per slot"
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT TRUE    # MIDI-learned knobs (Source/MidiLearn.*) drive the slots
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    COPY_PLUGIN_AFTER_BUILD FALSE
    PLUGIN_MANUFACTURER_CODE Pefx
    PLUGIN_CODE Efxg
    FORMATS VST3 Standalone
    PRODUCT_NAME "Pastel EFX Grid"
)

juce_generate_juce_header(PastelEfxGrid)

juce_add_binary_data(PastelAssets
    HEADER_NAME PastelAssets.h
    NAMESPACE PastelAssets
    SOURCES
        Assets/Lato-Regular.ttf
        Assets/Lato-Bold.ttf
        Assets/Lato-Black.ttf
)

# DSP + registry (shared by the plugin and the dev tools)
set(PASTEL_DSP_SOURCES
    Source/EffectRegistry.cpp
    Source/EffectRegistryImpl.cpp
    Source/dsp/Effects.cpp
    Source/dsp/fx/FxAnalysis.cpp
    Source/dsp/fx/FxDynamics.cpp
    Source/dsp/fx/FxEqVolume.cpp
    Source/dsp/fx/FxFilter.cpp
    Source/dsp/fx/FxModulation.cpp
    Source/dsp/fx/FxStereoPitch.cpp
    Source/dsp/fx/FxDrive.cpp
    Source/dsp/fx/FxSpace.cpp
    Source/dsp/fx/FxSpectralTime.cpp
)
set(PASTEL_UI_SOURCES
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp
    Source/Visuals.cpp
    Source/VizEditor.cpp
    Source/Presets.cpp
    Source/PresetBrowser.cpp
    Source/Theme.cpp
    Source/MidiLearn.cpp
)

target_sources(PastelEfxGrid PRIVATE ${PASTEL_UI_SOURCES} ${PASTEL_DSP_SOURCES})

target_include_directories(PastelEfxGrid PRIVATE Source Source/dsp)

target_compile_definitions(PastelEfxGrid PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_DISPLAY_SPLASH_SCREEN=0
)

target_link_libraries(PastelEfxGrid PRIVATE
    PastelAssets
    juce::juce_audio_utils
    juce::juce_dsp
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags
)

# ---- headless DSP self-test (dev tool, never shipped) ----
add_executable(GridTest TestGrid.cpp ${PASTEL_DSP_SOURCES})
target_include_directories(GridTest PRIVATE Source Source/dsp
    ${CMAKE_CURRENT_BINARY_DIR}/PastelEfxGrid_artefacts/JuceLibraryCode)
target_link_libraries(GridTest PRIVATE juce::juce_dsp juce::juce_audio_basics juce::juce_audio_formats juce::juce_graphics
    juce::juce_data_structures juce::juce_events juce::juce_core juce::juce_recommended_config_flags)
target_compile_definitions(GridTest PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
# GridTest only needs the JUCE modules, not the plugin target

# ---- offscreen editor renderer (dev tool, never shipped) ----
add_executable(EditorShot TestShot.cpp ${PASTEL_UI_SOURCES} ${PASTEL_DSP_SOURCES})
target_include_directories(EditorShot PRIVATE Source Source/dsp
    ${CMAKE_CURRENT_BINARY_DIR}/PastelEfxGrid_artefacts/JuceLibraryCode)
target_link_libraries(EditorShot PRIVATE PastelAssets juce::juce_audio_utils juce::juce_dsp
    juce::juce_recommended_config_flags)
target_compile_definitions(EditorShot PRIVATE
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_DISPLAY_SPLASH_SCREEN=0
    JUCE_STANDALONE_APPLICATION=1)
add_dependencies(EditorShot PastelEfxGrid)
