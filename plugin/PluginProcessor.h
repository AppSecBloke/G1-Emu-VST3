#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>
#include "g1Lib/g1mc.h"
#include <array>
#include <cmath>
#include <memory>
#include <mutex>
#include <vector>

class G1PluginProcessor final : public juce::AudioProcessor
{
public:
    G1PluginProcessor();
    ~G1PluginProcessor() override;
    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported(const BusesLayout&) const override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }
    const juce::String getName() const override { return "G1-Emu"; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}
    void getStateInformation(juce::MemoryBlock&) override;
    void setStateInformation(const void*, int) override;

    bool loadRom(const juce::File&, juce::String& error);
    juce::String romPath() const;
    juce::String status() const;
    g1::Microcontroller* machine() const { return mc.get(); }

private:
    void advanceTo(uint64_t targetCycles);
    void resetMachine(const std::vector<uint8_t>& rom, const std::vector<uint8_t>* flash = nullptr);
    static bool readAndValidateRom(const juce::File&, std::vector<uint8_t>&, juce::String&);

    std::unique_ptr<g1::Microcontroller> mc;
    std::vector<uint8_t> romBytes;
    juce::String currentRomPath;
    double hostRate = 48000.0;
    double emuTimeCycles = 0.0;
    float gain = std::pow(10.0f, 36.0f / 20.0f);
    std::vector<std::array<float,4>> native;
    std::mutex machineMutex;
    juce::String lastStatus = "Select a Nord Modular G1 512 KB ROM to begin.";
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(G1PluginProcessor)
};
