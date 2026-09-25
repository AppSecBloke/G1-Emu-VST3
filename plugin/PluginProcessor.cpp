#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "g1Lib/g1rom.h"
#include "model/ModuleDescriptions.h"
#include "model/PatchSerializer.h"
#include "model/PchFileIO.h"
#include "model/Patch.h"
#include <juce_cryptography/juce_cryptography.h>
#include <set>
#include <tuple>
#include "midi/UploadPacketizer.h"
#include <BinaryData.h>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#ifdef G1_DSP_TRACE
#include "g1Lib/g1_note_event_trace.h"
#include <fstream>
#endif
namespace{constexpr int32_t dc=0x155;constexpr float scale24=1.0f/8388608.0f;constexpr uint64_t g_ms=g1::g_ucClock/1000;void runFor(g1::Microcontroller&mc,uint64_t cycles){auto end=mc.ucCycles()+cycles;while(mc.ucCycles()<end)mc.exec();}std::vector<uint8_t>transact(g1::Microcontroller&mc,const std::vector<uint8_t>&msg,uint32_t timeoutMs=300){mc.getPcPort().receive(msg);std::vector<uint8_t>out;for(uint32_t t=0;t<timeoutMs;++t){runFor(mc,g_ms);mc.getPcPort().takeTx(out);if(!out.empty()&&out.back()==0xf7){runFor(mc,5*g_ms);mc.getPcPort().takeTx(out);return out;}}return out;}std::vector<uint8_t>checksum(std::vector<uint8_t>m){uint32_t s=0;for(auto b:m)s+=b;m.push_back((uint8_t)(s&0x7f));m.push_back(0xf7);return m;}}
namespace
{
    constexpr int legacyStateMagic = 0x47314531;
    constexpr int build7StateMagic = 0x47314537;
    constexpr int maxPatchBytes = 4 * 1024 * 1024;
    constexpr int maxStateBytes = 16 * 1024 * 1024;

#ifdef G1_DSP_TRACE
    void tracePatchLoad(const char* event, uint64_t call, const char* origin,
                        const juce::File& file, uint64_t cpuCycle, uint64_t dspCycle, int pid)
    {
        const char* path = std::getenv("G1_VST_PATCH_LOAD_TRACE_FILE");
        if (!path || !*path) return;
        std::ofstream out(path, std::ios::app);
        if (out)
            out << event << '\t' << call << '\t' << origin << '\t'
                << file.getFullPathName().toStdString() << '\t' << cpuCycle
                << '\t' << dspCycle << '\t' << pid << '\n';
    }
#endif

}

// A host state restore can overlap a message-thread load/panic. All operations
// share one pause lifetime; only the last exit restores the original state.
// Enter/exit outside machineMutex to preserve callback-lock -> machine-lock order.
struct G1PluginProcessor::ProcessingPause
{
    explicit ProcessingPause(G1PluginProcessor& p) : processor(p)
    {
        const juce::ScopedLock lock(processor.getCallbackLock());
        if (processor.processingPauseDepth++ == 0)
        {
            processor.processingWasSuspended = processor.isSuspended();
            processor.suspendProcessing(true);
        }
    }
    ~ProcessingPause()
    {
        const juce::ScopedLock lock(processor.getCallbackLock());
        jassert(processor.processingPauseDepth > 0);
        if (--processor.processingPauseDepth == 0)
            processor.suspendProcessing(processor.processingWasSuspended);
    }
    G1PluginProcessor& processor;
    JUCE_DECLARE_NON_COPYABLE(ProcessingPause)
};

G1PluginProcessor::G1PluginProcessor()
    : AudioProcessor(BusesProperties().withOutput("G1 Outputs", juce::AudioChannelSet::discreteChannels(4), true)),
      moduleDescriptions(std::make_unique<ModuleDescriptions>())
{
    oscCoarse = new juce::AudioParameterInt(juce::ParameterID{"oscCoarse", 1}, "Osc A Coarse", 0, 127, 64);
    addParameter(oscCoarse); // Keep the Build #6 host ID; all other controls are UI-only.
    const auto xml = juce::String::fromUTF8(reinterpret_cast<const char*>(BinaryData::modules_xml),
                                          static_cast<int>(BinaryData::modules_xmlSize));
    catalogueReady = moduleDescriptions->loadFromXmlString(xml);
    catalogueHash = juce::SHA256(BinaryData::modules_xml, BinaryData::modules_xmlSize).toHexString();
}
G1PluginProcessor::~G1PluginProcessor() = default;

void G1PluginProcessor::prepareToPlay(double sr,int){hostRate=sr>0?sr:48000.0;}void G1PluginProcessor::releaseResources(){}bool G1PluginProcessor::isBusesLayoutSupported(const BusesLayout&l)const{return l.getMainInputChannelSet().isDisabled()&&l.getMainOutputChannelSet().size()==4;}
bool G1PluginProcessor::readAndValidateRom(const juce::File&f,std::vector<uint8_t>&out,juce::String&error){if(!f.existsAsFile()){error="ROM file does not exist.";return false;}if(f.getSize()!=g1::g_romSize){error="The G1 ROM must be exactly 512 KB (524288 bytes).";return false;}juce::MemoryBlock b;if(!f.loadFileAsData(b)){error="Could not read the ROM file.";return false;}out.assign((const uint8_t*)b.getData(),(const uint8_t*)b.getData()+b.getSize());auto c=g1::checkRom(out);if(!c.ok()){error="Unsupported G1 ROM: "+juce::String(c.what());return false;}return true;}
void G1PluginProcessor::resetMachine(const std::vector<uint8_t>&rom,const std::vector<uint8_t>*flash){invalidatePatchLocked();mc=std::make_unique<g1::Microcontroller>(rom);if(flash&&flash->size()==g1::Flash::Size)mc->getFlash().data()=*flash;else mc->installRomOsInFlash();native.clear();panicMuted.store(false);currentPatchPid=-1;lastSentOscCoarse=oscCoarse->get();midiMessages=0;midiBytes=0;audioBlocks=0;for(auto&p:outputPeak)p=0;mc->getDsp(0).setInputProvider([](int32_t&l,int32_t&r){l=r=0;});mc->getDsp(3).setBlockCallback([this](int32_t a,int32_t b,int32_t c,int32_t d){std::array<int32_t,4>raw{a,b,c,d};for(size_t i=0;i<4;++i){auto v=raw[i]-dc;auto av=(uint32_t)(v<0?-static_cast<int64_t>(v):v);auto old=outputPeak[i].load();while(av>old&&!outputPeak[i].compare_exchange_weak(old,av)){} }++audioBlocks;native.push_back({(a-dc)*scale24*gain,(b-dc)*scale24*gain,(c-dc)*scale24*gain,(d-dc)*scale24*gain});});emuTimeCycles=0.0;}
bool G1PluginProcessor::loadRom(const juce::File& file, juce::String& error)
{
    std::vector<uint8_t> bytes;
    if (!readAndValidateRom(file, bytes, error))
    {
        std::lock_guard lock(machineMutex);
        lastStatus = error;
        return false;
    }
    ProcessingPause pause(*this);
    std::lock_guard lock(machineMutex);
    romBytes = std::move(bytes);
    currentRomPath = file.getFullPathName();
    resetMachine(romBytes);
    lastStatus = "ROM loaded. G1 is booting inside the plug-in.";
    error.clear();
    return true;
}

void G1PluginProcessor::invalidatePatchLocked()
{
    ++patchGeneration;
    patchParameters.clear(); // Also discards all coalesced, unsent edits.
    activePatch.reset();
    currentPatchPath.clear();
    sourcePatchHash.clear();
    currentPatchPid = -1;
    oscCoarseBinding = -1;
    lastSentOscCoarse = oscCoarse->get();
    sentLiveEdits = 0;
    lastEditAddress = {};
    lastEditValue = 0;
    nextEditCycle = 0;
    editCursor = 0;
}

bool G1PluginProcessor::discoverParametersLocked(Patch& patch,
                                                 std::vector<ParameterView>& views,
                                                 juce::String& error) const
{
    std::set<std::tuple<int, int, int>> addresses;
    for (int section : {0, 1})
        for (const auto& module : patch.getContainer(section).getModules())
        {
            const auto* md = module->getDescriptor();
            for (const auto& parameter : module->getParameters())
            {
                const auto* pd = parameter.getDescriptor();
                if (pd->paramClass != "parameter")
                    continue;
                // This ordinary-edit protocol has seven-bit identifiers and values.
                if (module->getContainerIndex() < 0 || module->getContainerIndex() > 127
                    || pd->index < 0 || pd->index > 127 || pd->minValue < 0
                    || pd->maxValue > 127 || pd->minValue >= pd->maxValue)
                    continue;
                ParameterView view;
                view.address = {section, module->getContainerIndex(), pd->index};
                if (!addresses.emplace(section, view.address.module, view.address.parameter).second)
                {
                    error = "Duplicate ordinary parameter address in patch metadata.";
                    return false;
                }
                view.moduleType = md->index;
                view.moduleTypeName = md->name;
                view.moduleTitle = module->getTitle();
                view.name = pd->name;
                view.componentId = pd->componentId;
                view.formatter = pd->formatter;
                view.minimum = pd->minValue;
                view.maximum = pd->maxValue;
                view.value = parameter.getValue();
                views.push_back(std::move(view));
            }
        }
    return true;
}

bool G1PluginProcessor::loadPatch(const juce::File& file, juce::String& error)
{
    ProcessingPause pause(*this);
    std::lock_guard lock(machineMutex);
    const bool ok = loadPatchLocked(file, error, nullptr, "button");
    if (!ok)
        lastStatus = error;
    return ok;
}

bool G1PluginProcessor::loadPatchLocked(const juce::File& file, juce::String& error,
                                       const juce::XmlElement* overlay, const char* origin)
{
#ifdef G1_DSP_TRACE
    const auto traceCall = ++patchLoadTraceSequence;
    tracePatchLoad("entry", traceCall, origin, file, mc ? mc->ucCycles() : 0,
                   mc ? mc->getDsp(0).dsp().getCycles() : 0, -1);
#else
    (void)origin;
#endif
    if (!mc) { error = "Load the ROM first."; return false; }
    if (!catalogueReady) { error = "Could not load embedded modules.xml."; return false; }
    juce::MemoryBlock before, after;
    if (!file.existsAsFile() || file.getSize() <= 0 || file.getSize() > maxPatchBytes
        || !file.loadFileAsData(before))
    {
        error = "Patch is missing, unreadable or larger than 4 MB.";
        return false;
    }
    PchFileIO io(*moduleDescriptions);
    auto candidate = io.readFile(file);
    // PchFileIO reads the file itself. Verify that its source stayed unchanged
    // across parsing before attaching an identity or restoring any saved values.
    if (!candidate || !file.loadFileAsData(after) || before != after)
    {
        error = "Could not parse the patch, or its file changed while loading.";
        return false;
    }
    const auto hash = juce::SHA256(before).toHexString();
    std::vector<ParameterView> views;
    if (!discoverParametersLocked(*candidate, views, error))
        return false;
    if (overlay != nullptr && !applyOverlayLocked(*candidate, views, hash, *overlay, error))
        return false;

    PatchSerializer serializer;
    const auto packets = UploadPacketizer::cut(serializer.serializeForUpload(*candidate));
    if (packets.empty()) { error = "The patch produced no upload packets."; return false; }

    // From here the OS can change. A failed/partial upload must not leave old
    // controls active against an uncertain patch or PID.
    invalidatePatchLocked();
    auto failed = [&](const juce::String& message)
    {
        error = message;
        native.clear();
        emuTimeCycles = static_cast<double>(mc->ucCycles());
        return false;
    };
    if (mc->ucCycles() < 1500 * g_ms)
        runFor(*mc, 1500 * g_ms - mc->ucCycles());
    std::vector<uint8_t> drain;
    mc->getPcPort().takeTx(drain);
    if (transact(*mc, {0xf0,0x33,0x00,0x06,0x00,0x03,0x03,0xf7}, 1000).empty())
        return failed("G1 OS did not answer the PC-Port handshake.");
    const std::vector<std::vector<uint8_t>>init={{0xf0,0x33,0x5c,0x06,0x41,0x14,0x00,0x00},{0xf0,0x33,0x5c,0x06,0x44,0x02,0x06,0x08,0x04},{0xf0,0x33,0x5c,0x06,0x41,0x35},{0xf0,0x33,0x5c,0x06,0x00,0x20,0x28},{0xf0,0x33,0x5c,0x06,0x00,0x4b,0x01},{0xf0,0x33,0x5c,0x06,0x00,0x4b,0x00},{0xf0,0x33,0x5c,0x06,0x00,0x53,0x01},{0xf0,0x33,0x5c,0x06,0x00,0x53,0x00},{0xf0,0x33,0x5c,0x06,0x00,0x4c,0x01},{0xf0,0x33,0x5c,0x06,0x00,0x4c,0x00},{0xf0,0x33,0x5c,0x06,0x00,0x66},{0xf0,0x33,0x5c,0x06,0x00,0x63},{0xf0,0x33,0x5c,0x06,0x00,0x61},{0xf0,0x33,0x5c,0x06,0x00,0x4e,0x01},{0xf0,0x33,0x5c,0x06,0x00,0x4e,0x00},{0xf0,0x33,0x5c,0x06,0x00,0x68}};
    for (auto message : init)
        transact(*mc, checksum(std::move(message)), 200);
    int pid = -1;
    for (size_t i = 0; i < packets.size(); ++i)
    {
        const auto reply = transact(*mc, UploadPacketizer::frame(packets[i], i == 0,
                                                                i + 1 == packets.size(), 0), 500);
        if (reply.empty())
            return failed("No reply to patch packet " + juce::String((int)i + 1)
                          + " of " + juce::String((int)packets.size()) + ".");
        for (size_t k = 0; k + 6 < reply.size(); ++k)
            if (reply[k] == 0xf0 && reply[k + 1] == 0x33
                && (reply[k + 2] >> 2) == 0x16 && reply[k + 5] == 0x36)
                pid = reply[k + 6];
    }
    if (pid < 0)
        return failed("The G1 OS did not confirm the uploaded patch.");
    runFor(*mc, 300 * g_ms);
    native.clear();
    panicMuted.store(false);
    emuTimeCycles = static_cast<double>(mc->ucCycles());
    currentPatchPath = file.getFullPathName();
    sourcePatchHash = hash;
    currentPatchPid = pid;
#ifdef G1_DSP_TRACE
    tracePatchLoad("complete", traceCall, origin, file, mc->ucCycles(),
                   mc->getDsp(0).dsp().getCycles(), pid);
#endif
    activePatch = std::move(candidate);
    int aliasMatches = 0;
    for (auto view : views)
    {
        const int original = view.value; // Value in the source file, before overlay.
        view.value = activePatch->getContainer(view.address.section)
                         .getModuleByIndex(view.address.module)->getParameter(view.address.parameter)->getValue();
        // Semantic compatibility lookup only. Never infer a PC-Port address
        // from a module type, list position, component-id or display order.
        if (view.moduleTypeName == "OscA" && view.name.equalsIgnoreCase("freq coarse")
            && view.minimum == 0 && view.maximum == 127)
        {
            ++aliasMatches;
            oscCoarseBinding = static_cast<int>(patchParameters.size());
        }
        patchParameters.push_back({std::move(view), original, false});
    }
    if (aliasMatches != 1)
        oscCoarseBinding = -1;
    else
        patchParameters[static_cast<size_t>(oscCoarseBinding)].view.hostAlias = true;
    // Loading a patch/state is not a host automation event. Establish a baseline
    // instead of applying the host default (or a value left by the previous patch).
    lastSentOscCoarse = oscCoarse->get();
    lastStatus = "Patch loaded: " + activePatch->getName() + " (slot 1, PID " + juce::String(pid) + ")";
    error.clear();
    return true;
}

G1PluginProcessor::ParameterSnapshot G1PluginProcessor::parameterSnapshot() const
{
    std::lock_guard lock(machineMutex);
    ParameterSnapshot snapshot;
    snapshot.generation = patchGeneration;
    for (const auto& record : patchParameters)
        snapshot.parameters.push_back(record.view);
    return snapshot;
}

bool G1PluginProcessor::queueEditLocked(uint64_t generation, ParameterAddress address, int value)
{
    if (generation != patchGeneration || !activePatch || currentPatchPid < 0)
        return false;
    for (auto& record : patchParameters)
        if (record.view.address == address)
        {
            auto* module = activePatch->getContainer(address.section).getModuleByIndex(address.module);
            auto* parameter = module ? module->getParameter(address.parameter) : nullptr;
            if (!parameter || parameter->getDescriptor()->paramClass != "parameter")
                return false;
            value = juce::jlimit(record.view.minimum, record.view.maximum, value);
            if (parameter->getValue() != value)
            {
                parameter->setValue(value);
                record.view.value = parameter->getValue();
                record.pending = true; // One pending value per address: latest wins.
            }
            return true;
        }
    return false;
}

bool G1PluginProcessor::editParameter(uint64_t generation, ParameterAddress address, int value)
{
    // These are custom UI edits. The legacy host parameter remains an input
    // alias; UI controls read the patch model, never an unrelated host default.
    std::lock_guard lock(machineMutex);
    captureHostParameterLocked();
    return queueEditLocked(generation, address, value);
}

void G1PluginProcessor::captureHostParameterLocked()
{
    const int value = oscCoarse->get();
    if (value == lastSentOscCoarse)
        return;
    lastSentOscCoarse = value;
    if (oscCoarseBinding >= 0)
        queueEditLocked(patchGeneration,
                        patchParameters[static_cast<size_t>(oscCoarseBinding)].view.address, value);
}

void G1PluginProcessor::deliverEditsLocked()
{
    if (!mc || !activePatch || patchParameters.empty() || mc->ucCycles() < nextEditCycle)
        return;
    for (size_t visited = 0; visited < patchParameters.size(); ++visited)
    {
        auto& record = patchParameters[editCursor];
        editCursor = (editCursor + 1) % patchParameters.size();
        if (!record.pending)
            continue;
        const auto address = record.view.address;
        sendLiveParameter(address.section, address.module, address.parameter, record.view.value);
        record.pending = false;
        ++sentLiveEdits;
        lastEditAddress = address;
        lastEditValue = record.view.value;
        // A 12-byte edit takes ~3.84 ms at MIDI baud. Space sends in emulated
        // time and never burst a backlog after a long host block.
        nextEditCycle = mc->ucCycles() + 5 * g_ms;
        break;
    }
}

void G1PluginProcessor::sendLiveParameter(int section,int module,int parameter,int value){if(!mc||currentPatchPid<0)return;std::vector<uint8_t>m{0xf0,0x33,0x4c,0x06,(uint8_t)(currentPatchPid&0x7f),0x40,(uint8_t)(section&0x7f),(uint8_t)(module&0x7f),(uint8_t)(parameter&0x7f),(uint8_t)(value&0x7f)};mc->getPcPort().receive(checksum(std::move(m)));}
void G1PluginProcessor::panic(){ProcessingPause pause(*this);std::lock_guard lock(machineMutex);if(mc){for(int ch=0;ch<16;++ch){const uint8_t st=(uint8_t)(0xB0|ch);mc->getSci().write(std::vector<uint8_t>{st,123,0});mc->getSci().write(std::vector<uint8_t>{st,120,0});}runFor(*mc,20*g_ms);native.clear();panicMuted.store(true);emuTimeCycles=(double)mc->ucCycles();lastStatus="Panic: all notes off and output muted.";}}
void G1PluginProcessor::advanceTo(uint64_t t){if(mc)while(mc->ucCycles()<t)mc->exec();}
void G1PluginProcessor::processBlock(juce::AudioBuffer<float>&buffer,juce::MidiBuffer&midi){juce::ScopedNoDenormals nd;buffer.clear();std::lock_guard lock(machineMutex);if(!mc)return;native.clear();captureHostParameterLocked();deliverEditsLocked();int n=buffer.getNumSamples(),cursor=0;for(const auto meta:midi){int pos=juce::jlimit(0,n,meta.samplePosition);emuTimeCycles+=(pos-cursor)*(double)g1::g_ucClock/hostRate;advanceTo((uint64_t)std::llround(emuTimeCycles));const auto msg=meta.getMessage();if(msg.isNoteOn())
{
    panicMuted.store(false);
#ifdef G1_DSP_TRACE
    if (!noteCompareArmed && currentPatchPid >= 0)
        if (const char* tracePath = std::getenv("G1_VST_NOTE_TRACE_FILE"))
        {
            noteCompareArmed = mc->getDsp(0).armNoteCompareTrace(tracePath, mc->ucCycles(), msg.getNoteNumber());
            if (!noteCompareArmed)
                lastStatus = "Could not open DSP0 note trace file.";
        }
#endif
}auto*p=msg.getRawData();int sz=msg.getRawDataSize();if(p&&sz>0){mc->getSci().write(std::vector<uint8_t>(p,p+sz));
#ifdef G1_DSP_TRACE
    g1::noteEventTrace().submit(mc.get(), &mc->getDsp(0).dsp(), mc->ucCycles(),
        mc->getDsp(0).dsp().getCycles(), p, sz,
        (msg.isNoteOn() || msg.isNoteOff()) ? msg.getNoteNumber() : -1, msg.isNoteOn());
#endif
    ++midiMessages;midiBytes.fetch_add((uint64_t)sz);}cursor=pos;}emuTimeCycles+=(n-cursor)*(double)g1::g_ucClock/hostRate;advanceTo((uint64_t)std::llround(emuTimeCycles));
#ifdef G1_DSP_TRACE
g1::noteEventTrace().finish(&mc->getDsp(0).dsp(), mc->getDsp(0).dsp().getCycles());
#endif
midi.clear();std::vector<uint8_t> pcDiscard;mc->getPcPort().takeTx(pcDiscard);if(panicMuted.load()){buffer.clear();return;}if(native.empty())return;for(int i=0;i<n;++i){double x=n>1?(double)i*(native.size()-1)/(n-1):0;size_t a=(size_t)x,b=std::min(a+1,native.size()-1);float t=(float)(x-a);for(int ch=0;ch<std::min(4,buffer.getNumChannels());++ch)buffer.setSample(ch,i,native[a][ch]+(native[b][ch]-native[a][ch])*t);}}
juce::String G1PluginProcessor::diagnostics(){std::lock_guard lock(machineMutex);if(!mc)return"ROM required";juce::String s;s<<"CPU: "<<juce::String((juce::int64)mc->ucCycles())<<" cycles | PIT: "<<juce::String((juce::int64)mc->pitIrqs())<<"\n";s<<"MIDI -> SCI: "<<juce::String((juce::int64)midiMessages.load())<<" msgs / "<<juce::String((juce::int64)midiBytes.load())<<" bytes | SCI reads: "<<(int)mc->sciDataReads()<<"\n";s<<"DSP booted/count: ";for(int i=0;i<4;++i){auto&d=mc->getDsp((uint32_t)i);s<<i<<":"<<(d.booted()?"Y":"N")<<"/"<<(int)d.bootCount()<<(i<3?"  ":"");}s<<"\nDSP IRQD: ";for(int i=0;i<4;++i)s<<i<<":"<<juce::String((juce::int64)mc->getDsp((uint32_t)i).irqdCount())<<(i<3?"  ":"");s<<"\nDSP3 frames: "<<juce::String((juce::int64)mc->getDsp(3).audioFrames())<<" | output blocks: "<<juce::String((juce::int64)audioBlocks.load())<<"\nOutput peak raw: ";for(int i=0;i<4;++i)s<<(i+1)<<":"<<(int)outputPeak[i].load()<<(i<3?"  ":"");s<<"\nOsc A coarse host alias: "<<(oscCoarseBinding>=0?"bound":"unbound")<<" | PID: "<<currentPatchPid;
s<<"\nDiscovered parameters: "<<(int)patchParameters.size()<<" | Sent live edits: "<<juce::String((juce::int64)sentLiveEdits);
if(sentLiveEdits>0)s<<"\nLast edit: section "<<lastEditAddress.section<<" / module "<<lastEditAddress.module<<" / parameter "<<lastEditAddress.parameter<<" / value "<<lastEditValue;
else s<<"\nLast edit: none";
return s;}
std::unique_ptr<juce::XmlElement> G1PluginProcessor::makeOverlayLocked() const
{
    auto overlay = std::make_unique<juce::XmlElement>("PatchOverlay");
    overlay->setAttribute("version", 1);
    overlay->setAttribute("sourceHash", sourcePatchHash);
    overlay->setAttribute("catalogueHash", catalogueHash);
    for (const auto& record : patchParameters)
    {
        const auto& view = record.view;
        if (view.value == record.originalValue)
            continue;
        auto* entry = overlay->createNewChildElement("Parameter");
        entry->setAttribute("section", view.address.section);
        entry->setAttribute("module", view.address.module);
        entry->setAttribute("parameter", view.address.parameter);
        entry->setAttribute("moduleType", view.moduleType);
        entry->setAttribute("componentId", view.componentId);
        entry->setAttribute("minimum", view.minimum);
        entry->setAttribute("maximum", view.maximum);
        entry->setAttribute("original", record.originalValue);
        entry->setAttribute("value", view.value);
    }
    return overlay;
}

bool G1PluginProcessor::applyOverlayLocked(Patch& patch, const std::vector<ParameterView>& views,
                                           const juce::String& sourceHash,
                                           const juce::XmlElement& overlay, juce::String& error) const
{
    if (!overlay.hasTagName("PatchOverlay") || overlay.getIntAttribute("version") != 1
        || overlay.getStringAttribute("sourceHash") != sourceHash
        || overlay.getStringAttribute("catalogueHash") != catalogueHash)
    {
        error = "Saved edits were not restored: the source patch or module catalogue changed.";
        return false;
    }
    std::set<std::tuple<int, int, int>> seen;
    // Validate the entire overlay before applying even its first entry.
    for (auto* entry = overlay.getFirstChildElement(); entry; entry = entry->getNextElement())
    {
        const ParameterAddress address{entry->getIntAttribute("section", -1),
                                       entry->getIntAttribute("module", -1),
                                       entry->getIntAttribute("parameter", -1)};
        const auto match = std::find_if(views.begin(), views.end(),
                                       [&](const auto& view) { return view.address == address; });
        if (!entry->hasTagName("Parameter") || match == views.end()
            || !seen.emplace(address.section, address.module, address.parameter).second
            || entry->getIntAttribute("moduleType", -1) != match->moduleType
            || entry->getStringAttribute("componentId") != match->componentId
            || entry->getIntAttribute("minimum", -1) != match->minimum
            || entry->getIntAttribute("maximum", -1) != match->maximum
            || entry->getIntAttribute("original", -1) != match->value
            || entry->getIntAttribute("value", -1) < match->minimum
            || entry->getIntAttribute("value", -1) > match->maximum)
        {
            error = "Saved edits contain an invalid or incompatible parameter.";
            return false;
        }
    }
    for (auto* entry = overlay.getFirstChildElement(); entry; entry = entry->getNextElement())
        patch.getContainer(entry->getIntAttribute("section"))
            .getModuleByIndex(entry->getIntAttribute("module"))
            ->getParameter(entry->getIntAttribute("parameter"))->setValue(entry->getIntAttribute("value"));
    return true;
}

void G1PluginProcessor::getStateInformation(juce::MemoryBlock& dest)
{
    std::lock_guard lock(machineMutex);
    // Capture automation received while the host was stopped, without sending.
    captureHostParameterLocked();
    juce::MemoryOutputStream stream(dest, false);
    stream.writeInt(build7StateMagic);
    stream.writeString(currentRomPath);
    if (mc)
    {
        const auto& flash = mc->getFlash().data();
        stream.writeInt(static_cast<int>(flash.size()));
        stream.write(flash.data(), flash.size());
    }
    else
        stream.writeInt(0);
    stream.writeString(currentPatchPath);
    stream.writeInt(oscCoarse->get());
    stream.writeString(makeOverlayLocked()->toString());
}

void G1PluginProcessor::setStateInformation(const void* data, int size)
{
    if (data == nullptr || size < 4 || size > maxStateBytes)
        return;
    juce::MemoryInputStream stream(data, static_cast<size_t>(size), false);
    const int magic = stream.readInt();
    if (magic != legacyStateMagic && magic != build7StateMagic)
        return;
    // JUCE strings are NUL-terminated UTF-8. Reject truncated strings instead
    // of interpreting the next field at an invented offset.
    auto readString = [&](juce::String& result)
    {
        const auto start = stream.getPosition();
        result = stream.readString();
        const auto end = stream.getPosition();
        return end > start && static_cast<const uint8_t*>(data)[static_cast<size_t>(end - 1)] == 0;
    };
    juce::String path, savedPatchPath, overlayText;
    if (!readString(path) || stream.getNumBytesRemaining() < 4)
        return;
    const int flashSize = stream.readInt();
    if ((flashSize != 0 && flashSize != static_cast<int>(g1::Flash::Size))
        || stream.getNumBytesRemaining() < flashSize)
        return;
    std::vector<uint8_t> flash(static_cast<size_t>(flashSize));
    if (flashSize > 0)
        stream.read(flash.data(), flash.size());
    if (stream.getNumBytesRemaining() > 0 && !readString(savedPatchPath))
        return;
    int savedCoarse = 64;
    if (magic == build7StateMagic || stream.getNumBytesRemaining() > 0)
    {
        if (stream.getNumBytesRemaining() < 4)
            return;
        savedCoarse = juce::jlimit(0, 127, stream.readInt());
    }
    std::unique_ptr<juce::XmlElement> overlay;
    if (magic == build7StateMagic)
    {
        if (!readString(overlayText))
            return;
        overlay = juce::XmlDocument::parse(overlayText);
        if (!overlay || !overlay->hasTagName("PatchOverlay") || overlay->getIntAttribute("version") != 1)
            return;
    }
    std::vector<uint8_t> bytes;
    juce::String error;
    if (path.isNotEmpty() && !readAndValidateRom(juce::File(path), bytes, error))
    {
        std::lock_guard lock(machineMutex);
        lastStatus = "Saved ROM could not be loaded: " + error;
        return;
    }

    ProcessingPause pause(*this);
    // Host notifications may call back into the processor: do not hold its mutex.
    oscCoarse->setValueNotifyingHost(oscCoarse->convertTo0to1(static_cast<float>(savedCoarse)));
    std::lock_guard lock(machineMutex);
    if (path.isEmpty())
    {
        invalidatePatchLocked();
        mc.reset();
        romBytes.clear();
        currentRomPath.clear();
        lastStatus = "Session restored without a ROM.";
        return;
    }
    romBytes = std::move(bytes);
    currentRomPath = path;
    resetMachine(romBytes, flash.empty() ? nullptr : &flash);
    lastStatus = "Session ROM state restored.";
    if (savedPatchPath.isNotEmpty())
    {
        // Overlay values become part of the upload itself, never post-load edits.
        if (!loadPatchLocked(juce::File(savedPatchPath), error, overlay.get(), "state_restore"))
            lastStatus = "Session ROM restored, but patch restoration failed: " + error;
        else
            lastStatus = "Session restored, including patch parameter values.";
    }
}
juce::String G1PluginProcessor::romPath() const
{ std::lock_guard lock(machineMutex); return currentRomPath; }
juce::String G1PluginProcessor::patchPath() const
{ std::lock_guard lock(machineMutex); return currentPatchPath; }
juce::String G1PluginProcessor::status() const
{ std::lock_guard lock(machineMutex); return lastStatus; }
juce::AudioProcessorEditor* G1PluginProcessor::createEditor() { return new G1PluginEditor(*this); }
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new G1PluginProcessor(); }
