cmake_minimum_required(VERSION 3.22)

# Universal binary (Apple Silicon + Intel) for the Mac beta. Before project() so it applies to
# every target; an explicit -DCMAKE_OSX_ARCHITECTURES=... still wins.
if(APPLE AND NOT CMAKE_OSX_ARCHITECTURES)
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "Target architectures" FORCE)
endif()

project(R3WRK VERSION 0.1.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(APPLE)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS version")
endif()

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../JUCE JUCE)

#===============================================================================
juce_add_plugin(R3WRK
    COMPANY_NAME "Heath Audio"
    BUNDLE_ID "com.heathaudio.r3wrk"
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT TRUE    # the fixed MIDI CC map (Source/MidiCcMap.h)
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS TRUE
    COPY_PLUGIN_AFTER_BUILD TRUE   # install VST3 + AU into ~/Library/Audio/Plug-Ins on every build
    # Without this, JUCE never adds NSMicrophoneUsageDescription to the Standalone app's
    # Info.plist -- macOS then silently denies mic access at the OS level (no prompt, no entry
    # in System Settings > Privacy > Microphone at all, just zeroed input samples), which is
    # what made Standalone recordings capture silence despite the right input device/channels
    # being selected in Audio Settings.
    MICROPHONE_PERMISSION_ENABLED TRUE
    MICROPHONE_PERMISSION_TEXT "R3WRK needs microphone access to record audio."
    PLUGIN_MANUFACTURER_CODE Heat
    PLUGIN_CODE R3wk
    FORMATS VST3 AU Standalone
    PRODUCT_NAME "R3WRK"
    ICON_BIG "${CMAKE_CURRENT_SOURCE_DIR}/Resources/AppIcon.png")

juce_generate_juce_header(R3WRK)

# Ad-hoc signed by default, so a fresh clone builds with zero setup -- but macOS ties the Screen
# Recording (TCC) grant to the exact code signature, and an ad-hoc signature is a hash of the
# binary itself, so it changes on every rebuild that touches source: each one needs Screen
# Recording re-approved from scratch (DesktopAudioCapture.mm / Black Box). To sign with a real
# Apple identity instead (stable across rebuilds -- same fix RCRDR/SHOWR/Sieve use via their
# Signing.local.xcconfig), copy Signing.local.cmake.example to Signing.local.cmake (gitignored)
# next to this file and fill in your own identity/team.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/Signing.local.cmake")
    include("${CMAKE_CURRENT_SOURCE_DIR}/Signing.local.cmake")
    foreach(r3wrk_signed_format VST3 AU Standalone)
        if(TARGET R3WRK_${r3wrk_signed_format})
            set_target_properties(R3WRK_${r3wrk_signed_format} PROPERTIES
                XCODE_ATTRIBUTE_CODE_SIGN_STYLE "Manual"
                XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY "${R3WRK_CODE_SIGN_IDENTITY}"
                XCODE_ATTRIBUTE_DEVELOPMENT_TEAM "${R3WRK_DEVELOPMENT_TEAM}")
        endif()
    endforeach()
endif()

# EXPERIMENT (branch experiment/space-mono-font, not on main): embeds the Space Mono
# Google Font (OFL) as binary data so R3WRKLookAndFeel::getTypefaceForFont can swap it in
# app-wide -- see that override for the rationale.
juce_add_binary_data(R3WRKFonts
    SOURCES
        Resources/Fonts/SpaceMono-Regular.ttf
        Resources/Fonts/SpaceMono-Bold.ttf)

target_sources(R3WRK
    PRIVATE
        Source/PluginProcessor.cpp
        Source/PluginEditor.cpp
        Source/AudioDocument.cpp
        Source/EditActions.cpp
        Source/WaveformDisplay.cpp
        Source/SpectrogramDisplay.cpp
        Source/TimeStretchEngine.cpp
        Source/HeaderBar.cpp
        Source/EditorToolbar.cpp
        Source/KnobRow.cpp
        Source/FxRow.cpp
        Source/LfoPanel.cpp
        Source/ReverbPanel.cpp
        Source/ShimmerPanel.cpp
        Source/PlexiphonPanel.cpp
        Source/MimeophonPanel.cpp
        Source/R3WRKLookAndFeel.cpp
        Source/DotMatrixLCD.cpp
        Source/TimeRuler.cpp
        Source/Theme.cpp
        Source/ThemeEditor.cpp
        Source/OutputSettings.cpp
        Source/OctatrackOtFile.cpp
        Source/StandaloneWindowShape.mm
        Source/DesktopAudioCapture.mm)

# DesktopAudioCapture.mm is ObjC++ against ScreenCaptureKit -- ARC-managed, and the
# framework is weak-linked so an ad-hoc build still launches on hosts without it.
set_source_files_properties(Source/DesktopAudioCapture.mm PROPERTIES
    COMPILE_FLAGS "-fobjc-arc")

target_compile_definitions(R3WRK
    PUBLIC
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0
        JUCE_DISPLAY_SPLASH_SCREEN=0
        JUCE_USE_LAME_AUDIO_FORMAT=1)   # MP3 export via a system `lame` binary (see AudioDocument::findLameBinary)

target_link_libraries(R3WRK
    PRIVATE
        juce::juce_audio_utils
        juce::juce_audio_processors
        juce::juce_audio_formats
        juce::juce_audio_devices
        juce::juce_dsp
        juce::juce_gui_extra
        R3WRKFonts
        "-framework CoreMedia"
        "-weak_framework ScreenCaptureKit"
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

#===============================================================================
# Headless smoke test for the editing engine (AudioDocument/EditActions/TimeStretch)
# -- no GUI, no audio device, safe to run in CI or a sandboxed build.
juce_add_console_app(R3WRKSmokeTest PRODUCT_NAME "R3WRKSmokeTest")
juce_generate_juce_header(R3WRKSmokeTest)

target_sources(R3WRKSmokeTest
    PRIVATE
        Source/AudioDocument.cpp
        Source/EditActions.cpp
        Source/OctatrackOtFile.cpp
        Source/TimeStretchEngine.cpp
        Source/Theme.cpp
        Tests/SmokeTest.cpp)

target_compile_definitions(R3WRKSmokeTest
    PUBLIC
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_USE_LAME_AUDIO_FORMAT=1)   # AudioDocument.cpp references LAMEEncoderAudioFormat

target_link_libraries(R3WRKSmokeTest
    PRIVATE
        juce::juce_core
        juce::juce_data_structures
        juce::juce_events
        juce::juce_audio_basics
        juce::juce_audio_formats
        juce::juce_audio_processors   # HostParams.h (DocKnobParam)
        juce::juce_gui_basics          # Theme.cpp (Colour, SystemClipboard)
    PUBLIC
        juce::juce_recommended_config_flags)
