if(NOT G1_HAVE_JUCE)
    message(STATUS "G1 VST3: JUCE not available")
    return()
endif()

juce_add_plugin(g1vst3
    COMPANY_NAME "Animatek"
    BUNDLE_ID "com.animatek.g1emu"
    PLUGIN_MANUFACTURER_CODE Anmk
    PLUGIN_CODE G1Em
    FORMATS VST3
    PRODUCT_NAME "G1-Emu"
    IS_SYNTH TRUE
    NEEDS_MIDI_INPUT TRUE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    COPY_PLUGIN_AFTER_BUILD FALSE)

target_sources(g1vst3 PRIVATE PluginProcessor.cpp PluginProcessor.h PluginEditor.cpp PluginEditor.h)
target_include_directories(g1vst3 PRIVATE ${CMAKE_SOURCE_DIR})
target_compile_definitions(g1vst3 PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_VST3_CAN_REPLACE_VST2=0)
target_link_libraries(g1vst3 PRIVATE g1Lib juce::juce_audio_utils juce::juce_recommended_config_flags juce::juce_recommended_lto_flags)
