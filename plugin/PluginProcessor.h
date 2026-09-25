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

class ModuleDescriptions;
class Patch;

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
    juce::AudioParameterInt* oscCoarseParameter() const{return oscCoarse;}

    struct ParameterAddress
    {
        int section = 0, module = 0, parameter = 0;
        bool operator==(const ParameterAddress& other) const
        { return section == other.section && module == other.module && parameter == other.parameter; }
    };
    // Copies only: an editor must never retain NME pointers across a patch replacement.
    struct ParameterView
    {
        ParameterAddress address;
        int moduleType = 0, minimum = 0, maximum = 127, value = 0;
        juce::String moduleTitle, moduleTypeName, name, componentId, formatter;
        bool hostAlias = false;
    };
    struct ParameterSnapshot
    {
        uint64_t generation = 0;
        std::vector<ParameterView> parameters;
    };
    ParameterSnapshot parameterSnapshot() const;
    bool editParameter(uint64_t generation, ParameterAddress address, int rawValue);
private:
    struct ProcessingPause;
    void advanceTo(uint64_t); void resetMachine(const std::vector<uint8_t>&,const std::vector<uint8_t>* =nullptr);
    void sendLiveParameter(int,int,int,int);
    static bool readAndValidateRom(const juce::File&,std::vector<uint8_t>&,juce::String&);
    bool loadPatchLocked(const juce::File&, juce::String&, const juce::XmlElement* overlay = nullptr);
    void invalidatePatchLocked();
    bool discoverParametersLocked(Patch&, std::vector<ParameterView>&, juce::String&) const;
    bool applyOverlayLocked(Patch&, const std::vector<ParameterView>&,
                            const juce::String& sourceHash, const juce::XmlElement&, juce::String&) const;
    bool queueEditLocked(uint64_t, ParameterAddress, int);
    void captureHostParameterLocked();
    void deliverEditsLocked();
    std::unique_ptr<juce::XmlElement> makeOverlayLocked() const;

    // The catalogue is loaded once and outlives every Patch and its descriptor pointers.
    std::unique_ptr<ModuleDescriptions> moduleDescriptions;
    std::unique_ptr<Patch> activePatch;
    bool catalogueReady = false;
    juce::String catalogueHash, sourcePatchHash;
    struct ParameterRecord { ParameterView view; int originalValue = 0; bool pending = false; };
    std::vector<ParameterRecord> patchParameters;
    uint64_t patchGeneration = 0, sentLiveEdits = 0, nextEditCycle = 0;
    size_t editCursor = 0;
    int oscCoarseBinding = -1;
    ParameterAddress lastEditAddress;
    int lastEditValue = 0;
    std::unique_ptr<g1::Microcontroller> mc; std::vector<uint8_t> romBytes; juce::String currentRomPath,currentPatchPath;
#ifdef G1_DSP_TRACE
    bool noteCompareArmed = false;
#endif
    double hostRate=48000.0,emuTimeCycles=0.0; float gain=std::pow(10.0f,36.0f/20.0f);
    std::vector<std::array<float,4>> native; mutable std::mutex machineMutex;
    // Guarded by JUCE's callback lock, never by machineMutex.
    int processingPauseDepth = 0;
    bool processingWasSuspended = false;
    std::atomic<bool> panicMuted{false};
    int currentPatchPid=-1,lastSentOscCoarse=-1;
    juce::AudioParameterInt* oscCoarse=nullptr;
    juce::String lastStatus="Select a Nord Modular G1 512 KB ROM to begin.";
    std::atomic<uint64_t> midiMessages{0},midiBytes{0},audioBlocks{0}; std::array<std::atomic<uint32_t>,4> outputPeak{};
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(G1PluginProcessor)
};
