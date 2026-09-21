#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include "PluginProcessor.h"
class G1PluginEditor final:public juce::AudioProcessorEditor,private juce::Timer{
public:explicit G1PluginEditor(G1PluginProcessor&);void paint(juce::Graphics&)override;void resized()override;
private:void timerCallback()override;G1PluginProcessor&proc;juce::TextButton chooseRom{"Choose ROM..."},loadPatch{"Load Patch..."},panicButton{"PANIC"};juce::Label title,rom,patch,status,diag;std::unique_ptr<juce::FileChooser>chooser;
};
