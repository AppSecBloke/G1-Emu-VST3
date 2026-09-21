#pragma once
#include <JuceHeader.h>
#include "PluginProcessor.h"
class G1PluginEditor final : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit G1PluginEditor(G1PluginProcessor&);
    void paint(juce::Graphics&) override;
    void resized() override;
private:
    void timerCallback() override;
    G1PluginProcessor& proc;
    juce::TextButton choose{"Choose ROM…"};
    juce::Label title, rom, status, lcd;
    std::unique_ptr<juce::FileChooser> chooser;
};
