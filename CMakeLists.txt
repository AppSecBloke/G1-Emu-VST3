if(NOT G1_HAVE_JUCE)
    message(STATUS "G1 VST3: JUCE not available")
    return()
endif()

set(NME_DIR "${CMAKE_SOURCE_DIR}/../Animatek-NME" CACHE PATH "Animatek NME tree")
if(NOT EXISTS "${NME_DIR}/source/model/PatchSerializer.cpp")
    message(FATAL_ERROR "G1 VST3 patch loading needs Animatek NME at NME_DIR=${NME_DIR}")
endif()

# Embed the module catalogue in the plug-in so installed builds do not depend
# on the editor source tree from the build machine.
juce_add_binary_data(G1Vst3Data SOURCES
    ${NME_DIR}/data/modules.xml)

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

target_sources(g1vst3 PRIVATE
    PluginProcessor.cpp PluginProcessor.h PluginEditor.cpp PluginEditor.h
    ${NME_DIR}/source/model/Patch.cpp
    ${NME_DIR}/source/model/ModuleDescriptions.cpp
    ${NME_DIR}/source/model/ModuleTags.cpp
    ${NME_DIR}/source/model/ThemeData.cpp
    ${NME_DIR}/source/format/ValueFormatters.cpp
    ${NME_DIR}/source/model/PatchSerializer.cpp
    ${NME_DIR}/source/model/PatchParser.cpp
    ${NME_DIR}/source/model/PchFileIO.cpp
    ${NME_DIR}/source/midi/UploadPacketizer.cpp
    ${NME_DIR}/source/midi/SysExCodec.cpp)

target_include_directories(g1vst3 PRIVATE ${CMAKE_SOURCE_DIR} ${NME_DIR}/source)
target_compile_definitions(g1vst3 PRIVATE
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_VST3_CAN_REPLACE_VST2=0)
target_link_libraries(g1vst3 PRIVATE
    g1Lib G1Vst3Data
    juce::juce_audio_utils
    juce::juce_recommended_config_flags
    juce::juce_recommended_lto_flags)
