#include "PluginEditor.h"
G1PluginEditor::G1PluginEditor(G1PluginProcessor&p):AudioProcessorEditor(&p),proc(p){
 title.setText("G1-Emu - Nord Modular G1 emulator - playability build #6",juce::dontSendNotification);title.setFont(juce::FontOptions(20.0f,juce::Font::bold));
 rom.setText("ROM: "+(proc.romPath().isEmpty()?juce::String("not selected"):proc.romPath()),juce::dontSendNotification);
 patch.setText("Patch: "+(proc.patchPath().isEmpty()?juce::String("none"):proc.patchPath()),juce::dontSendNotification);status.setText(proc.status(),juce::dontSendNotification);
 diag.setFont(juce::FontOptions(15.0f));diag.setJustificationType(juce::Justification::topLeft);diag.setColour(juce::Label::textColourId,juce::Colours::black);
 for(auto*c:{(juce::Component*)&title,(juce::Component*)&rom,(juce::Component*)&patch,(juce::Component*)&status,(juce::Component*)&diag,(juce::Component*)&chooseRom,(juce::Component*)&loadPatch,(juce::Component*)&panicButton})addAndMakeVisible(c);
 chooseRom.onClick=[this]{chooser=std::make_unique<juce::FileChooser>("Choose your Nord Modular G1 512 KB ROM",juce::File{},"*");chooser->launchAsync(juce::FileBrowserComponent::openMode|juce::FileBrowserComponent::canSelectFiles,[this](const juce::FileChooser&fc){auto f=fc.getResult();if(f==juce::File{})return;juce::String e;proc.loadRom(f,e);rom.setText("ROM: "+proc.romPath(),juce::dontSendNotification);patch.setText("Patch: none",juce::dontSendNotification);status.setText(e.isEmpty()?proc.status():e,juce::dontSendNotification);});};
 loadPatch.onClick=[this]{chooser=std::make_unique<juce::FileChooser>("Load Nord Modular patch",juce::File{},"*.pch");chooser->launchAsync(juce::FileBrowserComponent::openMode|juce::FileBrowserComponent::canSelectFiles,[this](const juce::FileChooser&fc){auto f=fc.getResult();if(f==juce::File{})return;juce::String e;proc.loadPatch(f,e);patch.setText("Patch: "+(proc.patchPath().isEmpty()?juce::String("none"):proc.patchPath()),juce::dontSendNotification);status.setText(e.isEmpty()?proc.status():e,juce::dontSendNotification);});};
 panicButton.onClick=[this]{proc.panic();status.setText(proc.status(),juce::dontSendNotification);};
 oscLabel.setText("Osc A Coarse", juce::dontSendNotification);
 oscLabel.setJustificationType(juce::Justification::centred);
 oscLabel.setColour(juce::Label::textColourId, juce::Colours::black);
 addAndMakeVisible(oscLabel);
 oscCoarse.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
 oscCoarse.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 72, 24);
 oscCoarse.setRange(0.0, 127.0, 1.0);
 oscCoarse.setNumDecimalPlacesToDisplay(0);
 oscCoarse.setName("Osc A Coarse");
 oscCoarse.setColour(juce::Slider::rotarySliderFillColourId, juce::Colour(0xffa52020));
 oscCoarse.setColour(juce::Slider::textBoxTextColourId, juce::Colours::black);
 oscCoarse.setColour(juce::Slider::textBoxBackgroundColourId, juce::Colours::white);
 addAndMakeVisible(oscCoarse);
 oscCoarseAttachment = std::make_unique<juce::SliderParameterAttachment>(
     *proc.oscCoarseParameter(), oscCoarse, nullptr);
 setSize(780,540);startTimerHz(4);
}
void G1PluginEditor::paint(juce::Graphics&g){g.fillAll(juce::Colour(0xffd9d9d5));g.setColour(juce::Colours::black);g.drawRect(getLocalBounds(),2);}
void G1PluginEditor::resized(){auto r=getLocalBounds().reduced(18);title.setBounds(r.removeFromTop(32));auto buttons=r.removeFromTop(36);chooseRom.setBounds(buttons.removeFromLeft(130));buttons.removeFromLeft(10);loadPatch.setBounds(buttons.removeFromLeft(130));buttons.removeFromLeft(10);panicButton.setBounds(buttons.removeFromLeft(100));rom.setBounds(r.removeFromTop(26));patch.setBounds(r.removeFromTop(26));status.setBounds(r.removeFromTop(28));r.removeFromTop(8);
 auto oscillator = r.removeFromTop(160).removeFromLeft(160);
 oscLabel.setBounds(oscillator.removeFromTop(24));
 oscCoarse.setBounds(oscillator);
 r.removeFromTop(10);
 diag.setBounds(r);
}
void G1PluginEditor::timerCallback(){diag.setText(proc.diagnostics(),juce::dontSendNotification);}
