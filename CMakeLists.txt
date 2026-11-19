cmake_minimum_required(VERSION 3.22)
project(SIDIMIDI VERSION 2.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(JUCE_DIR "$ENV{USERPROFILE}/JUCE" CACHE PATH "Path to the JUCE source tree")
add_subdirectory(${JUCE_DIR} JUCE)

# Everything without user interface (also used by the console test tool)
set(SM_CORE
    Source/SidInfo.cpp
    Source/Hvsc.cpp
    Source/Archive.cpp
    Source/SidCpu6502Engine.cpp
    Source/SidTrace.cpp)

set(SM_DEFS
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_DISPLAY_SPLASH_SCREEN=0
    SM_VERSION_STRING="${PROJECT_VERSION}")

# Core test (console): header parsing, HVSC documents, ...
juce_add_console_app(SidTest PRODUCT_NAME "SidTest" COMPANY_NAME "Un(d)AbtanzBAR")
target_sources(SidTest PRIVATE Tools/SidTest.cpp Source/EmbeddedMiniplayer.cpp ${SM_CORE})
juce_generate_juce_header(SidTest)
target_compile_definitions(SidTest PRIVATE ${SM_DEFS})
if (MSVC)
    target_compile_options(SidTest PRIVATE /utf-8)
endif()
target_link_libraries(SidTest PRIVATE juce::juce_core juce::juce_events juce::juce_data_structures juce::juce_cryptography juce::juce_audio_utils
    PUBLIC juce::juce_recommended_config_flags)

# ------------------------------------------------------------------------------------------------
# The application
set(SM_UI
    Source/Lang.cpp
    Source/UpdateCheck.cpp
    Source/EmbeddedMiniplayer.cpp
    Source/SidTable.cpp
    Source/SidView.cpp
    Source/SidTab.cpp
    Source/MidiTab.cpp
    Source/Panels.cpp
    Source/MainComponent.cpp)

juce_add_binary_data(SidBinary SOURCES Resources/icon.png Resources/banner.png)

juce_add_gui_app(SIDIMIDI
    PRODUCT_NAME "SIDiMIDI"
    COMPANY_NAME "Un(d)AbtanzBAR"
    VERSION ${PROJECT_VERSION}
    ICON_BIG "${CMAKE_CURRENT_SOURCE_DIR}/Resources/icon.png"
    ICON_SMALL "${CMAKE_CURRENT_SOURCE_DIR}/Resources/icon_64.png")

juce_generate_juce_header(SIDIMIDI)
target_sources(SIDIMIDI PRIVATE Source/Main.cpp ${SM_CORE} ${SM_UI})
target_compile_definitions(SIDIMIDI PRIVATE ${SM_DEFS})
if (MSVC)
    target_compile_options(SIDIMIDI PRIVATE /utf-8)
endif()
target_link_libraries(SIDIMIDI
    PRIVATE
        SidBinary
        juce::juce_audio_utils
        juce::juce_cryptography
        juce::juce_gui_extra
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

# Renders the window to PNG files (UI checks without clicking)
juce_add_gui_app(UiShot PRODUCT_NAME "UiShot" COMPANY_NAME "Un(d)AbtanzBAR")
juce_generate_juce_header(UiShot)
target_sources(UiShot PRIVATE Tools/UiShot.cpp ${SM_CORE} ${SM_UI})
target_compile_definitions(UiShot PRIVATE ${SM_DEFS} JUCE_MODAL_LOOPS_PERMITTED=1)
if (MSVC)
    target_compile_options(UiShot PRIVATE /utf-8)
endif()
target_link_libraries(UiShot PRIVATE SidBinary juce::juce_audio_utils juce::juce_cryptography juce::juce_gui_extra PUBLIC juce::juce_recommended_config_flags)
