cmake_minimum_required(VERSION 3.22)
project(EpsilonPhon VERSION 1.1.0 LANGUAGES C CXX)

# macOS: 12.0 target and JUCE 9.0.5 (bundled in ./JUCE in the Projucer package; CI clones it).
set(CMAKE_OSX_DEPLOYMENT_TARGET "12.0" CACHE STRING "Minimum macOS version")

add_subdirectory(JUCE)
set(phon_formats VST3 Standalone)
if(APPLE)
    list(APPEND phon_formats AU)
endif()

juce_add_plugin(EpsilonPhon
    COMPANY_NAME "Epsilon Sounds"
    BUNDLE_ID com.epsilonsounds.phon
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    COPY_PLUGIN_AFTER_BUILD FALSE
    PLUGIN_MANUFACTURER_CODE EpsS
    PLUGIN_CODE EpPh
    VST3_CATEGORIES Fx Analyzer EQ
    AU_MAIN_TYPE kAudioUnitType_Effect
    FORMATS ${phon_formats}
    PRODUCT_NAME "Epsilon Phon"
    ICON_BIG Resources/AppIcon.png
    DESCRIPTION "Equal-loudness mix referee: measures your mix against hit records"
)
juce_generate_juce_header(EpsilonPhon)
juce_add_binary_data(PhonAssets SOURCES
    Resources/EpsilonDial-Regular.ttf
    Resources/EpsilonDial-Bold.ttf
)
set_target_properties(PhonAssets PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_sources(EpsilonPhon PRIVATE
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp
)
target_include_directories(EpsilonPhon PRIVATE Source Shared/epsilon_ui)
target_compile_features(EpsilonPhon PRIVATE cxx_std_17)
target_compile_definitions(EpsilonPhon PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
)
target_link_libraries(EpsilonPhon PRIVATE
    PhonAssets
    juce::juce_audio_utils
    juce::juce_recommended_config_flags
    juce::juce_recommended_lto_flags
    juce::juce_recommended_warning_flags
)

# Engine tests (no JUCE needed): cmake --build build --target PhonEngineTests && ctest
option(PHON_BUILD_TESTS "Build the engine tests" ON)
if(PHON_BUILD_TESTS)
    enable_testing()
    add_executable(PhonEngineTests Tests/EngineTests.cpp)
    target_include_directories(PhonEngineTests PRIVATE Source)
    target_compile_features(PhonEngineTests PRIVATE cxx_std_17)
    add_test(NAME PhonEngineTests COMMAND PhonEngineTests)
endif()

# Headless host test + editor snapshot. Off by default:
#   cmake -DPHON_BUILD_BENCH=ON ... && cmake --build build --target PhonBench
option(PHON_BUILD_BENCH "Build the host test / snapshot bench" OFF)
if(PHON_BUILD_BENCH)
    juce_add_console_app(PhonBench PRODUCT_NAME "PhonBench")
    target_sources(PhonBench PRIVATE Tests/PhonBench.cpp Source/PluginProcessor.cpp Source/PluginEditor.cpp)
    juce_generate_juce_header(PhonBench)
    target_include_directories(PhonBench PRIVATE Source Shared/epsilon_ui)
    target_compile_features(PhonBench PRIVATE cxx_std_17)
    target_compile_definitions(PhonBench PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_MODAL_LOOPS_PERMITTED=1
        JucePlugin_VersionString="${PROJECT_VERSION}" JucePlugin_Name="Epsilon Phon")
    target_link_libraries(PhonBench PRIVATE PhonAssets juce::juce_audio_utils juce::juce_recommended_config_flags)
endif()
