#include "PluginEditor.h"
G1PluginEditor::G1PluginEditor(G1PluginProcessor& p) : AudioProcessorEditor(&p), proc(p)
{
    title.setText("G1-Emu - Nord Modular G1 emulator - diagnostic build #2", juce::dontSendNotification); title.setFont(juce::FontOptions(20.0f, juce::Font::bold));
    rom.setText("ROM: " + (proc.romPath().isEmpty()?juce::String("not selected"):proc.romPath()),juce::dontSendNotification); status.setText(proc.status(),juce::dontSendNotification);
    diag.setFont(juce::FontOptions(15.0f)); diag.setJustificationType(juce::Justification::topLeft); diag.setColour(juce::Label::textColourId,juce::Colours::black);
    for(auto* c:{(juce::Component*)&title,(juce::Component*)&rom,(juce::Component*)&status,(juce::Component*)&diag,(juce::Component*)&choose})addAndMakeVisible(c);
    choose.onClick=[this]{chooser=std::make_unique<juce::FileChooser>("Choose your Nord Modular G1 512 KB ROM",juce::File{},"*");chooser->launchAsync(juce::FileBrowserComponent::openMode|juce::FileBrowserComponent::canSelectFiles,[this](const juce::FileChooser& fc){const auto f=fc.getResult();if(f==juce::File{})return;juce::String e;proc.loadRom(f,e);rom.setText("ROM: "+proc.romPath(),juce::dontSendNotification);status.setText(e.isEmpty()?proc.status():e,juce::dontSendNotification);});};
    setSize(760,330); startTimerHz(4);
}
void G1PluginEditor::paint(juce::Graphics& g){g.fillAll(juce::Colour(0xffd9d9d5));g.setColour(juce::Colours::black);g.drawRect(getLocalBounds(),2);}
void G1PluginEditor::resized(){auto r=getLocalBounds().reduced(18);title.setBounds(r.removeFromTop(32));choose.setBounds(r.removeFromTop(34).removeFromLeft(130));rom.setBounds(r.removeFromTop(28));status.setBounds(r.removeFromTop(28));r.removeFromTop(8);diag.setBounds(r);}
void G1PluginEditor::timerCallback(){diag.setText(proc.diagnostics(),juce::dontSendNotification);}
