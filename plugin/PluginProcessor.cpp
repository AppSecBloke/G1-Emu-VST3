#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "g1Lib/g1rom.h"
#include <algorithm>
#include <cmath>

namespace { constexpr int32_t dc = 0x155; constexpr double nativeRate = 96000.0; constexpr float scale24 = 1.0f / 8388608.0f; }

G1PluginProcessor::G1PluginProcessor()
: AudioProcessor(BusesProperties().withOutput("G1 Outputs", juce::AudioChannelSet::discreteChannels(4), true)) {}
G1PluginProcessor::~G1PluginProcessor() = default;

void G1PluginProcessor::prepareToPlay(double sr, int) { hostRate = sr > 0 ? sr : 48000.0; }
void G1PluginProcessor::releaseResources() {}

bool G1PluginProcessor::isBusesLayoutSupported(const BusesLayout& l) const
{
    return l.getMainInputChannelSet().isDisabled() && l.getMainOutputChannelSet().size() == 4;
}

bool G1PluginProcessor::readAndValidateRom(const juce::File& f, std::vector<uint8_t>& out, juce::String& error)
{
    if(!f.existsAsFile()) { error = "ROM file does not exist."; return false; }
    if(f.getSize() != g1::g_romSize) { error = "The G1 ROM must be exactly 512 KB (524288 bytes)."; return false; }
    juce::MemoryBlock b;
    if(!f.loadFileAsData(b)) { error = "Could not read the ROM file."; return false; }
    out.assign(static_cast<const uint8_t*>(b.getData()), static_cast<const uint8_t*>(b.getData()) + b.getSize());
    const auto check = g1::checkRom(out);
    if(!check.ok()) { error = juce::String("This does not look like a supported G1 ROM: ") + check.what(); return false; }
    return true;
}

void G1PluginProcessor::resetMachine(const std::vector<uint8_t>& rom, const std::vector<uint8_t>* flash)
{
    mc = std::make_unique<g1::Microcontroller>(rom);
    if(flash && flash->size() == g1::Flash::Size) mc->getFlash().data() = *flash;
    else mc->installRomOsInFlash();
    native.clear(); midiMessages = 0; midiBytes = 0; audioBlocks = 0;
    for(auto& p : outputPeak) p = 0;
    mc->getDsp(0).setInputProvider([](int32_t& l, int32_t& r){ l = r = 0; });
    mc->getDsp(3).setBlockCallback([this](int32_t a, int32_t b, int32_t c, int32_t d)
    {
        const std::array<int32_t,4> raw{a,b,c,d};
        for(size_t i=0;i<4;++i) {
            const auto v = raw[i] - dc; const auto av = static_cast<uint32_t>(v < 0 ? -static_cast<int64_t>(v) : v);
            auto old = outputPeak[i].load(); while(av > old && !outputPeak[i].compare_exchange_weak(old,av)) {}
        }
        ++audioBlocks;
        native.push_back({(a-dc)*scale24*gain, (b-dc)*scale24*gain, (c-dc)*scale24*gain, (d-dc)*scale24*gain});
    });
    emuTimeCycles = 0.0;
}

bool G1PluginProcessor::loadRom(const juce::File& f, juce::String& error)
{
    std::vector<uint8_t> bytes;
    if(!readAndValidateRom(f, bytes, error)) return false;
    suspendProcessing(true);
    { std::lock_guard lock(machineMutex); romBytes = bytes; currentRomPath = f.getFullPathName(); resetMachine(romBytes); lastStatus = "ROM loaded. G1 is booting inside the plug-in."; }
    suspendProcessing(false); return true;
}

void G1PluginProcessor::advanceTo(uint64_t target) { if(mc) while(mc->ucCycles() < target) mc->exec(); }

void G1PluginProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals; buffer.clear(); std::lock_guard lock(machineMutex); if(!mc) return;
    native.clear(); const int n=buffer.getNumSamples(); int cursor=0;
    for(const auto meta : midi) {
        const int pos=juce::jlimit(0,n,meta.samplePosition); emuTimeCycles += (pos-cursor)*(double)g1::g_ucClock/hostRate; advanceTo((uint64_t)std::llround(emuTimeCycles));
        const auto& msg=meta.getMessage(); const auto* p=msg.getRawData(); const int sz=msg.getRawDataSize();
        if(p && sz>0) { mc->getSci().write(std::vector<uint8_t>(p,p+sz)); ++midiMessages; midiBytes += (uint64_t)sz; } cursor=pos;
    }
    emuTimeCycles += (n-cursor)*(double)g1::g_ucClock/hostRate; advanceTo((uint64_t)std::llround(emuTimeCycles)); midi.clear();
    if(native.empty()) return;
    for(int i=0;i<n;++i) { const double x=n>1?(double)i*(native.size()-1)/(double)(n-1):0.0; const size_t a=(size_t)x,b=std::min(a+1,native.size()-1); const float t=(float)(x-(double)a); for(int ch=0;ch<std::min(4,buffer.getNumChannels());++ch) buffer.setSample(ch,i,native[a][ch]+(native[b][ch]-native[a][ch])*t); }
}

juce::String G1PluginProcessor::diagnostics()
{
    std::lock_guard lock(machineMutex); if(!mc) return "ROM required";
    juce::String s; s << "CPU: " << juce::String((juce::int64)mc->ucCycles()) << " cycles | PIT: " << juce::String((juce::int64)mc->pitIrqs()) << "\n";
    s << "MIDI -> SCI: " << juce::String((juce::int64)midiMessages.load()) << " msgs / " << juce::String((juce::int64)midiBytes.load()) << " bytes | SCI reads: " << (int)mc->sciDataReads() << "\n";
    s << "DSP booted/count: "; for(int i=0;i<4;++i) { auto& d=mc->getDsp((uint32_t)i); s << i << ":" << (d.booted()?"Y":"N") << "/" << (int)d.bootCount(); if(i<3)s << "  "; } s << "\n";
    s << "DSP IRQD: "; for(int i=0;i<4;++i) { s << i << ":" << juce::String((juce::int64)mc->getDsp((uint32_t)i).irqdCount()); if(i<3)s << "  "; } s << "\n";
    s << "DSP3 frames: " << juce::String((juce::int64)mc->getDsp(3).audioFrames()) << " | output blocks: " << juce::String((juce::int64)audioBlocks.load()) << "\n";
    s << "Output peak raw: "; for(int i=0;i<4;++i) { s << (i+1) << ":" << (int)outputPeak[i].load(); if(i<3)s << "  "; }
    return s;
}

void G1PluginProcessor::getStateInformation(juce::MemoryBlock& dest) { std::lock_guard lock(machineMutex); juce::MemoryOutputStream s(dest,false); s.writeInt(0x47314531); s.writeString(currentRomPath); if(mc){const auto& f=mc->getFlash().data();s.writeInt((int)f.size());s.write(f.data(),f.size());}else s.writeInt(0);}
void G1PluginProcessor::setStateInformation(const void* data,int size) { juce::MemoryInputStream s(data,(size_t)size,false); if(s.readInt()!=0x47314531)return; const auto path=s.readString(); const int flashSize=s.readInt(); std::vector<uint8_t> flash; if(flashSize==(int)g1::Flash::Size&&s.getNumBytesRemaining()>=flashSize){flash.resize((size_t)flashSize);s.read(flash.data(),flash.size());} if(path.isEmpty())return; std::vector<uint8_t> bytes;juce::String err;if(!readAndValidateRom(juce::File(path),bytes,err)){lastStatus="Saved ROM could not be loaded: "+err;return;} std::lock_guard lock(machineMutex);romBytes=std::move(bytes);currentRomPath=path;resetMachine(romBytes,flash.empty()?nullptr:&flash);lastStatus="Session state restored.";}
juce::String G1PluginProcessor::romPath() const{return currentRomPath;} juce::String G1PluginProcessor::status() const{return lastStatus;} juce::AudioProcessorEditor* G1PluginProcessor::createEditor(){return new G1PluginEditor(*this);} juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter(){return new G1PluginProcessor();}
