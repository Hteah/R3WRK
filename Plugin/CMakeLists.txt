cmake_minimum_required(VERSION 3.22)
project(R3WRK VERSION 0.1.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(APPLE)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS version")
endif()

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../JUCE JUCE)

#===============================================================================
# RubberBand (offline time-stretch / pitch-shift).
# RubberBand ships no CMake build — but it does ship `single/RubberBandSingle.cpp`,
# an amalgamated unit that pulls in every source with the right config (vDSP FFT +
# BQ resampler on Apple, no threads). We fetch the source and compile that one file
# into a small static lib. A system copy via pkg-config wins if one exists.
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(RUBBERBAND QUIET rubberband)
endif()

if(RUBBERBAND_FOUND)
    message(STATUS "R3WRK: using system RubberBand ${RUBBERBAND_VERSION}")
    add_library(rubberband_dep INTERFACE)
    target_include_directories(rubberband_dep INTERFACE ${RUBBERBAND_INCLUDE_DIRS})
    target_link_directories(rubberband_dep INTERFACE ${RUBBERBAND_LIBRARY_DIRS})
    target_link_libraries(rubberband_dep INTERFACE ${RUBBERBAND_LIBRARIES})
else()
    message(STATUS "R3WRK: building RubberBand v3.3.0 from its single-file unit")
    include(FetchContent)
    FetchContent_Declare(rubberband_src
        GIT_REPOSITORY https://github.com/breakfastquay/rubberband.git
        GIT_TAG        v3.3.0
        GIT_SHALLOW    TRUE)
    FetchContent_MakeAvailable(rubberband_src)   # no CMakeLists → just populates

    add_library(rubberband_dep STATIC ${rubberband_src_SOURCE_DIR}/single/RubberBandSingle.cpp)
    set_target_properties(rubberband_dep PROPERTIES POSITION_INDEPENDENT_CODE ON)
    target_compile_features(rubberband_dep PRIVATE cxx_std_17)
    target_include_directories(rubberband_dep
        PUBLIC  ${rubberband_src_SOURCE_DIR}            # <rubberband/RubberBandStretcher.h>
        PRIVATE ${rubberband_src_SOURCE_DIR}/src)
    if(APPLE)
        target_link_libraries(rubberband_dep PUBLIC "-framework Accelerate")
    endif()
endif()

#===============================================================================
juce_add_plugin(R3WRK
    COMPANY_NAME "Heath Audio"
    BUNDLE_ID "com.heathaudio.r3wrk"
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS TRUE
    COPY_PLUGIN_AFTER_BUILD FALSE
    PLUGIN_MANUFACTURER_CODE Heat
    PLUGIN_CODE R3wk
    FORMATS VST3 AU Standalone
    PRODUCT_NAME "R3WRK")

juce_generate_juce_header(R3WRK)

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
        Source/TimeRuler.cpp)

target_compile_definitions(R3WRK
    PUBLIC
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0
        JUCE_DISPLAY_SPLASH_SCREEN=0)

target_link_libraries(R3WRK
    PRIVATE
        juce::juce_audio_utils
        juce::juce_audio_processors
        juce::juce_audio_formats
        juce::juce_audio_devices
        juce::juce_dsp
        juce::juce_gui_extra
        rubberband_dep
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
        Source/TimeStretchEngine.cpp
        Tests/SmokeTest.cpp)

target_compile_definitions(R3WRKSmokeTest
    PUBLIC
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0)

target_link_libraries(R3WRKSmokeTest
    PRIVATE
        juce::juce_core
        juce::juce_data_structures
        juce::juce_events
        juce::juce_audio_basics
        juce::juce_audio_formats
        rubberband_dep
    PUBLIC
        juce::juce_recommended_config_flags)
