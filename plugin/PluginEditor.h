#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include "PluginProcessor.h"

class G1PluginEditor final : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit G1PluginEditor(G1PluginProcessor&);
    ~G1PluginEditor() override;
    void paint(juce::Graphics&) override;
    void resized() override;

private:
    class ParameterPanel;
    void timerCallback() override;
    void refresh();
    G1PluginProcessor& proc;
    juce::TextButton chooseRom{"Choose ROM..."}, loadPatch{"Load Patch..."}, panicButton{"PANIC"};
    juce::Label title, rom, patch, status, diag;
    std::unique_ptr<ParameterPanel> parameterPanel;
    juce::Viewport parameterViewport; // Non-owning; destroyed before the panel.
    std::unique_ptr<juce::FileChooser> chooser;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(G1PluginEditor)
};
