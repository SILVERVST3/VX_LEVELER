cmake_minimum_required(VERSION 3.22)
project(VX_Leveler_SILVER VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if (MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

include(FetchContent)

# ---------------------------------------------------------------- JUCE
# Usa ./JUCE se existir (clonado pelo workflow); senão baixa via FetchContent.
if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/JUCE/CMakeLists.txt")
    add_subdirectory(JUCE)
else()
    FetchContent_Declare(JUCE
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG        8.0.4
        GIT_SHALLOW    TRUE)
    FetchContent_MakeAvailable(JUCE)
endif()

# ---------------------------------------------------------------- ARA SDK
# Precisa das pastas ARA_API e ARA_Library (Celemony ARA SDK 2.2.0).
if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/ARA_SDK/ARA_API")
    set(VX_ARA_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/ARA_SDK")
else()
    FetchContent_Declare(ARA_SDK
        GIT_REPOSITORY https://github.com/Celemony/ARA_SDK.git
        GIT_TAG        releases/2.2.0
        GIT_SUBMODULES "ARA_API;ARA_Library")
    FetchContent_Populate(ARA_SDK)
    set(VX_ARA_SDK_DIR "${ara_sdk_SOURCE_DIR}")
endif()
juce_set_ara_sdk_path("${VX_ARA_SDK_DIR}")

# ---------------------------------------------------------------- Plugin
juce_add_plugin(VX_Leveler_SILVER
    COMPANY_NAME                "SILVER Audio"
    COMPANY_WEBSITE             "https://github.com/"
    BUNDLE_ID                   "com.silveraudio.vxleveler"
    PLUGIN_MANUFACTURER_CODE    Slvr
    PLUGIN_CODE                 VxLv
    PRODUCT_NAME                "VX_Leveler SILVER"
    FORMATS                     VST3
    IS_SYNTH                    FALSE
    NEEDS_MIDI_INPUT            FALSE
    NEEDS_MIDI_OUTPUT           FALSE
    IS_MIDI_EFFECT              FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS TRUE
    COPY_PLUGIN_AFTER_BUILD     FALSE
    VST3_CATEGORIES             Fx Dynamics
    # ---- ARA 2
    IS_ARA_EFFECT               TRUE
    ARA_ANALYSIS_TYPES          ""
    ARA_TRANSFORMATION_FLAGS    kARAPlaybackTransformationNoChanges)

target_sources(VX_Leveler_SILVER PRIVATE
    Source/LevelerDSP.cpp
    Source/VXDocumentController.cpp
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp
    Source/SilverLookAndFeel.cpp)

target_include_directories(VX_Leveler_SILVER PRIVATE Source)

target_compile_definitions(VX_Leveler_SILVER PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_DISPLAY_SPLASH_SCREEN=0)

target_link_libraries(VX_Leveler_SILVER
    PRIVATE
        juce::juce_audio_utils
        juce::juce_audio_processors
        juce::juce_audio_formats
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

# ---------------------------------------------------------------- teste offline do algoritmo (opcional)
option(VX_BUILD_TESTS "Compila o teste offline do nivelador" OFF)
if (VX_BUILD_TESTS)
    add_executable(vx_leveler_test Tests/LevelerTest.cpp Source/LevelerDSP.cpp)
    target_include_directories(vx_leveler_test PRIVATE Source)
    target_compile_definitions(vx_leveler_test PRIVATE VX_STANDALONE_DSP=1)
endif()

option(VX_BUILD_SNAPSHOT "Ferramenta de desenvolvimento: captura PNG do editor" OFF)
if (VX_BUILD_SNAPSHOT)
    add_executable(vx_ui_snapshot Tests/UISnapshot.cpp "${JUCE_SOURCE_DIR}/modules/juce_audio_plugin_client/juce_audio_plugin_client_ARA.cpp")
    target_compile_definitions(vx_ui_snapshot PRIVATE $<TARGET_PROPERTY:VX_Leveler_SILVER,COMPILE_DEFINITIONS>)
    target_include_directories(vx_ui_snapshot PRIVATE $<TARGET_PROPERTY:VX_Leveler_SILVER,INCLUDE_DIRECTORIES>)
    target_link_libraries(vx_ui_snapshot PRIVATE VX_Leveler_SILVER)
endif()
