#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>
#include "g1Lib/g1mc.h"
#include <array>
#include <atomic>
#include <cmath>
#include <memory>
#include <mutex>
#include <vector>

class G1PluginProcessor final : public juce::AudioProcessor
{
public:
    G1PluginProcessor(); ~G1PluginProcessor() override;
    void prepareToPlay(double,int) override; void releaseResources() override;
    bool isBusesLayoutSupported(const BusesLayout&) const override;
    void processBlock(juce::AudioBuffer<float>&,juce::MidiBuffer&) override;
    juce::AudioProcessorEditor* createEditor() override; bool hasEditor() const override{return true;}
    const juce::String getName() const override{return "G1-Emu";}
    bool acceptsMidi() const override{return true;} bool producesMidi() const override{return false;} bool isMidiEffect() const override{return false;}
    double getTailLengthSeconds() const override{return 0.0;} int getNumPrograms() override{return 1;} int getCurrentProgram() override{return 0;}
    void setCurrentProgram(int) override{} const juce::String getProgramName(int) override{return{};} void changeProgramName(int,const juce::String&) override{}
    void getStateInformation(juce::MemoryBlock&) override; void setStateInformation(const void*,int) override;
    bool loadRom(const juce::File&,juce::String&); bool loadPatch(const juce::File&,juce::String&); void panic();
    juce::String romPath() const; juce::String patchPath() const; juce::String status() const; juce::String diagnostics();
private:
    void advanceTo(uint64_t); void resetMachine(const std::vector<uint8_t>&,const std::vector<uint8_t>* =nullptr);
    static bool readAndValidateRom(const juce::File&,std::vector<uint8_t>&,juce::String&);
    std::unique_ptr<g1::Microcontroller> mc; std::vector<uint8_t> romBytes; juce::String currentRomPath,currentPatchPath;
    double hostRate=48000.0,emuTimeCycles=0.0; float gain=std::pow(10.0f,36.0f/20.0f);
    std::vector<std::array<float,4>> native; std::mutex machineMutex;
    juce::String lastStatus="Select a Nord Modular G1 512 KB ROM to begin.";
    std::atomic<uint64_t> midiMessages{0},midiBytes{0},audioBlocks{0}; std::array<std::atomic<uint32_t>,4> outputPeak{};
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(G1PluginProcessor)
};
