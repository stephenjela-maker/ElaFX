cmake_minimum_required(VERSION 3.20)
project(ELAFX VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)
FetchContent_Declare(
    JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG master
)
FetchContent_MakeAvailable(JUCE)

juce_add_plugin(ELAFX
    COMPANY_NAME "YourStudio"
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    FORMATS VST3
    PRODUCT_NAME "ELA FX"
)

target_sources(ELAFX PRIVATE PluginProcessor.cpp)

target_compile_definitions(ELAFX PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
)

target_link_libraries(ELAFX PRIVATE
juce::juce_dsp
)
