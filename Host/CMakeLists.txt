cmake_minimum_required(VERSION 3.22)
project(PixSpectraEngine VERSION 1.0.0 LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
include(SoftwareUI.cmake)
add_subdirectory(../third_party/JUCE JUCE)
add_library(PixSpectraEngine SHARED Main.cpp ../Protocol.h ../EngineAPI.h)
target_compile_definitions(PixSpectraEngine PRIVATE JUCE_GLOBAL_MODULE_SETTINGS_INCLUDED=1
    JUCE_PLUGINHOST_VST3=1 JUCE_PLUGINHOST_VST=0 JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0
    JUCE_MODAL_LOOPS_PERMITTED=1 PIXSPECTRA_SOFTWARE_UI=1)
target_link_libraries(PixSpectraEngine PRIVATE juce::juce_audio_utils juce::juce_dsp
    juce::juce_recommended_config_flags)
target_compile_options(PixSpectraEngine PRIVATE /utf-8)
add_executable(PixSpectraHarness ../tests/ae_harness.cpp)
target_compile_definitions(PixSpectraHarness PRIVATE MSWindows WIN32)
target_compile_options(PixSpectraHarness PRIVATE /utf-8)
target_include_directories(PixSpectraHarness PRIVATE ../../../Headers ../../../Headers/SP ../../../Headers/Win ../../../Util ../../../Resources)
add_executable(PixSpectraWindowWaitTest ../tests/window_wait_test.cpp)
target_link_libraries(PixSpectraWindowWaitTest PRIVATE user32)
