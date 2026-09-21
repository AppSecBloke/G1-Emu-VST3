#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "g1Lib/g1rom.h"
#include "model/ModuleDescriptions.h"
#include "model/PatchSerializer.h"
#include "model/PchFileIO.h"
#include "midi/UploadPacketizer.h"
#include <BinaryData.h>
#include <algorithm>
#include <cmath>

namespace {
constexpr int32_t dc=0x155; constexpr float scale24=1.0f/8388608.0f;
constexpr uint64_t g_ms=g1::g_ucClock/1000;
void runFor(g1::Microcontroller& mc,uint64_t cycles){auto end=mc.ucCycles()+cycles;while(mc.ucCycles()<end)mc.exec();}
std::vector<uint8_t> transact(g1::Microcontroller& mc,const std::vector<uint8_t>& msg,uint32_t timeoutMs=300){
    mc.getPcPort().receive(msg); std::vector<uint8_t> out;
    for(uint32_t t=0;t<timeoutMs;++t){runFor(mc,g_ms);mc.getPcPort().takeTx(out);if(!out.empty()&&out.back()==0xf7){runFor(mc,5*g_ms);mc.getPcPort().takeTx(out);return out;}}
    return out;
}
std::vector<uint8_t> checksum(std::vector<uint8_t> m){uint32_t s=0;for(auto b:m)s+=b;m.push_back((uint8_t)(s&0x7f));m.push_back(0xf7);return m;}
}

G1PluginProcessor::G1PluginProcessor():AudioProcessor(BusesProperties().withOutput("G1 Outputs",juce::AudioChannelSet::discreteChannels(4),true)){}
G1PluginProcessor::~G1PluginProcessor()=default;
void G1PluginProcessor::prepareToPlay(double sr,int){hostRate=sr>0?sr:48000.0;} void G1PluginProcessor::releaseResources(){}
bool G1PluginProcessor::isBusesLayoutSupported(const BusesLayout& l)const{return l.getMainInputChannelSet().isDisabled()&&l.getMainOutputChannelSet().size()==4;}
bool G1PluginProcessor::readAndValidateRom(const juce::File& f,std::vector<uint8_t>& out,juce::String& error){
 if(!f.existsAsFile()){error="ROM file does not exist.";return false;} if(f.getSize()!=g1::g_romSize){error="The G1 ROM must be exactly 512 KB (524288 bytes).";return false;}
 juce::MemoryBlock b;if(!f.loadFileAsData(b)){error="Could not read the ROM file.";return false;}out.assign((const uint8_t*)b.getData(),(const uint8_t*)b.getData()+b.getSize());
 auto c=g1::checkRom(out);if(!c.ok()){error="Unsupported G1 ROM: "+juce::String(c.what());return false;}return true;
}
void G1PluginProcessor::resetMachine(const std::vector<uint8_t>& rom,const std::vector<uint8_t>* flash){
 mc=std::make_unique<g1::Microcontroller>(rom);if(flash&&flash->size()==g1::Flash::Size)mc->getFlash().data()=*flash;else mc->installRomOsInFlash();
 native.clear();midiMessages=0;midiBytes=0;audioBlocks=0;for(auto& p:outputPeak)p=0;
 mc->getDsp(0).setInputProvider([](int32_t&l,int32_t&r){l=r=0;});
 mc->getDsp(3).setBlockCallback([this](int32_t a,int32_t b,int32_t c,int32_t d){std::array<int32_t,4> raw{a,b,c,d};for(size_t i=0;i<4;++i){auto v=raw[i]-dc;auto av=(uint32_t)(v<0?-static_cast<int64_t>(v):v);auto old=outputPeak[i].load();while(av>old&&!outputPeak[i].compare_exchange_weak(old,av)){} }++audioBlocks;native.push_back({(a-dc)*scale24*gain,(b-dc)*scale24*gain,(c-dc)*scale24*gain,(d-dc)*scale24*gain});});
 emuTimeCycles=0.0;
}
bool G1PluginProcessor::loadRom(const juce::File& f,juce::String& error){std::vector<uint8_t>b;if(!readAndValidateRom(f,b,error))return false;suspendProcessing(true);{std::lock_guard lock(machineMutex);romBytes=b;currentRomPath=f.getFullPathName();currentPatchPath.clear();resetMachine(romBytes);lastStatus="ROM loaded. G1 is booting inside the plug-in.";}suspendProcessing(false);return true;}

bool G1PluginProcessor::loadPatch(const juce::File& f,juce::String& error){
 if(!f.existsAsFile()){error="Patch file does not exist.";return false;}
 suspendProcessing(true); std::lock_guard lock(machineMutex);
 if(!mc){error="Load the ROM first.";suspendProcessing(false);return false;}
 ModuleDescriptions descs;
 const juce::String modulesXml = juce::String::fromUTF8(
     reinterpret_cast<const char*>(BinaryData::modules_xml),
     static_cast<int>(BinaryData::modules_xmlSize));
 if(!descs.loadFromXmlString(modulesXml)){error="Could not load embedded modules.xml.";suspendProcessing(false);return false;}
 PchFileIO io(descs);auto patch=io.readFile(f);if(!patch){error="Editor code could not parse this .pch file.";suspendProcessing(false);return false;}
 PatchSerializer ser;const auto packets=UploadPacketizer::cut(ser.serializeForUpload(*patch));
 if(mc->ucCycles()<1500*g_ms)runFor(*mc,1500*g_ms-mc->ucCycles());std::vector<uint8_t> drain;mc->getPcPort().takeTx(drain);
 if(transact(*mc,{0xf0,0x33,0x00,0x06,0x00,0x03,0x03,0xf7},1000).empty()){error="G1 OS did not answer the PC-Port handshake.";native.clear();emuTimeCycles=(double)mc->ucCycles();suspendProcessing(false);return false;}
 const std::vector<std::vector<uint8_t>> init={
 {0xf0,0x33,0x5c,0x06,0x41,0x14,0x00,0x00},{0xf0,0x33,0x5c,0x06,0x44,0x02,0x06,0x08,0x04},{0xf0,0x33,0x5c,0x06,0x41,0x35},
 {0xf0,0x33,0x5c,0x06,0x00,0x20,0x28},{0xf0,0x33,0x5c,0x06,0x00,0x4b,0x01},{0xf0,0x33,0x5c,0x06,0x00,0x4b,0x00},
 {0xf0,0x33,0x5c,0x06,0x00,0x53,0x01},{0xf0,0x33,0x5c,0x06,0x00,0x53,0x00},{0xf0,0x33,0x5c,0x06,0x00,0x4c,0x01},
 {0xf0,0x33,0x5c,0x06,0x00,0x4c,0x00},{0xf0,0x33,0x5c,0x06,0x00,0x66},{0xf0,0x33,0x5c,0x06,0x00,0x63},
 {0xf0,0x33,0x5c,0x06,0x00,0x61},{0xf0,0x33,0x5c,0x06,0x00,0x4e,0x01},{0xf0,0x33,0x5c,0x06,0x00,0x4e,0x00},{0xf0,0x33,0x5c,0x06,0x00,0x68}};
 for(auto m:init)transact(*mc,checksum(std::move(m)),200);
 int pid=-1;
 for(size_t i=0;i<packets.size();++i){auto reply=transact(*mc,UploadPacketizer::frame(packets[i],i==0,i+1==packets.size(),0),500);
   if(reply.empty()){error="No reply to patch packet "+juce::String((int)i+1)+" of "+juce::String((int)packets.size())+".";native.clear();emuTimeCycles=(double)mc->ucCycles();suspendProcessing(false);return false;}
   for(size_t k=0;k+6<reply.size();++k)if(reply[k]==0xf0&&reply[k+1]==0x33&&(reply[k+2]>>2)==0x16&&reply[k+5]==0x36)pid=reply[k+6];
 }
 if(pid<0){error="The G1 OS did not confirm the uploaded patch.";native.clear();emuTimeCycles=(double)mc->ucCycles();suspendProcessing(false);return false;}
 runFor(*mc,300*g_ms);native.clear();emuTimeCycles=(double)mc->ucCycles();currentPatchPath=f.getFullPathName();
 lastStatus="Patch loaded: "+patch->getName()+" (slot 1, PID "+juce::String(pid)+")";error.clear();suspendProcessing(false);return true;
}
void G1PluginProcessor::advanceTo(uint64_t t){if(mc)while(mc->ucCycles()<t)mc->exec();}
void G1PluginProcessor::processBlock(juce::AudioBuffer<float>& buffer,juce::MidiBuffer& midi){juce::ScopedNoDenormals nd;buffer.clear();std::lock_guard lock(machineMutex);if(!mc)return;native.clear();int n=buffer.getNumSamples(),cursor=0;for(const auto meta:midi){int pos=juce::jlimit(0,n,meta.samplePosition);emuTimeCycles+=(pos-cursor)*(double)g1::g_ucClock/hostRate;advanceTo((uint64_t)std::llround(emuTimeCycles));const auto msg=meta.getMessage();auto*p=msg.getRawData();int sz=msg.getRawDataSize();if(p&&sz>0){mc->getSci().write(std::vector<uint8_t>(p,p+sz));++midiMessages;midiBytes.fetch_add((uint64_t)sz);}cursor=pos;}emuTimeCycles+=(n-cursor)*(double)g1::g_ucClock/hostRate;advanceTo((uint64_t)std::llround(emuTimeCycles));midi.clear();if(native.empty())return;for(int i=0;i<n;++i){double x=n>1?(double)i*(native.size()-1)/(n-1):0;size_t a=(size_t)x,b=std::min(a+1,native.size()-1);float t=(float)(x-a);for(int ch=0;ch<std::min(4,buffer.getNumChannels());++ch)buffer.setSample(ch,i,native[a][ch]+(native[b][ch]-native[a][ch])*t);}}
juce::String G1PluginProcessor::diagnostics(){std::lock_guard lock(machineMutex);if(!mc)return"ROM required";juce::String s;s<<"CPU: "<<juce::String((juce::int64)mc->ucCycles())<<" cycles | PIT: "<<juce::String((juce::int64)mc->pitIrqs())<<"\n";s<<"MIDI -> SCI: "<<juce::String((juce::int64)midiMessages.load())<<" msgs / "<<juce::String((juce::int64)midiBytes.load())<<" bytes | SCI reads: "<<(int)mc->sciDataReads()<<"\n";s<<"DSP booted/count: ";for(int i=0;i<4;++i){auto&d=mc->getDsp((uint32_t)i);s<<i<<":"<<(d.booted()?"Y":"N")<<"/"<<(int)d.bootCount()<<(i<3?"  ":"");}s<<"\nDSP IRQD: ";for(int i=0;i<4;++i)s<<i<<":"<<juce::String((juce::int64)mc->getDsp((uint32_t)i).irqdCount())<<(i<3?"  ":"");s<<"\nDSP3 frames: "<<juce::String((juce::int64)mc->getDsp(3).audioFrames())<<" | output blocks: "<<juce::String((juce::int64)audioBlocks.load())<<"\nOutput peak raw: ";for(int i=0;i<4;++i)s<<(i+1)<<":"<<(int)outputPeak[i].load()<<(i<3?"  ":"");return s;}
void G1PluginProcessor::getStateInformation(juce::MemoryBlock& dest){std::lock_guard lock(machineMutex);juce::MemoryOutputStream s(dest,false);s.writeInt(0x47314531);s.writeString(currentRomPath);if(mc){auto&f=mc->getFlash().data();s.writeInt((int)f.size());s.write(f.data(),f.size());}else s.writeInt(0);}
void G1PluginProcessor::setStateInformation(const void*data,int size){juce::MemoryInputStream s(data,(size_t)size,false);if(s.readInt()!=0x47314531)return;auto path=s.readString();int fs=s.readInt();std::vector<uint8_t>flash;if(fs==(int)g1::Flash::Size&&s.getNumBytesRemaining()>=fs){flash.resize((size_t)fs);s.read(flash.data(),flash.size());}if(path.isEmpty())return;std::vector<uint8_t>b;juce::String e;if(!readAndValidateRom(juce::File(path),b,e)){lastStatus="Saved ROM could not be loaded: "+e;return;}std::lock_guard lock(machineMutex);romBytes=std::move(b);currentRomPath=path;resetMachine(romBytes,flash.empty()?nullptr:&flash);lastStatus="Session state restored.";}
juce::String G1PluginProcessor::romPath()const{return currentRomPath;}juce::String G1PluginProcessor::patchPath()const{return currentPatchPath;}juce::String G1PluginProcessor::status()const{return lastStatus;}juce::AudioProcessorEditor*G1PluginProcessor::createEditor(){return new G1PluginEditor(*this);}juce::AudioProcessor*JUCE_CALLTYPE createPluginFilter(){return new G1PluginProcessor();}
