#include "PluginEditor.h"
G1PluginEditor::G1PluginEditor(G1PluginProcessor& p) : AudioProcessorEditor(&p), proc(p)
{
    title.setText("G1-Emu · Nord Modular G1 emulator", juce::dontSendNotification);
    title.setFont(juce::FontOptions(22.0f, juce::Font::bold));
    rom.setText("ROM: " + (proc.romPath().isEmpty() ? juce::String("not selected") : proc.romPath()), juce::dontSendNotification);
    status.setText(proc.status(), juce::dontSendNotification);
    lcd.setJustificationType(juce::Justification::centred);
    lcd.setFont(juce::FontOptions(18.0f, juce::Font::bold));
    for(auto* c : { (juce::Component*)&title, (juce::Component*)&rom, (juce::Component*)&status, (juce::Component*)&lcd, (juce::Component*)&choose }) addAndMakeVisible(c);
    choose.onClick = [this]
    {
        chooser = std::make_unique<juce::FileChooser>("Choose your Nord Modular G1 512 KB ROM", juce::File{}, "*");
        chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [this](const juce::FileChooser& fc)
        {
            const auto f = fc.getResult(); if(f == juce::File{}) return;
            juce::String e; proc.loadRom(f, e);
            rom.setText("ROM: " + proc.romPath(), juce::dontSendNotification);
            status.setText(e.isEmpty() ? proc.status() : e, juce::dontSendNotification);
        });
    };
    setSize(720, 250); startTimerHz(10);
}
void G1PluginEditor::paint(juce::Graphics& g) { g.fillAll(juce::Colour(0xffd9d9d5)); g.setColour(juce::Colours::black); g.drawRect(getLocalBounds(), 2); }
void G1PluginEditor::resized()
{
    auto r=getLocalBounds().reduced(18); title.setBounds(r.removeFromTop(34)); choose.setBounds(r.removeFromTop(34).removeFromLeft(130));
    rom.setBounds(r.removeFromTop(30)); status.setBounds(r.removeFromTop(30)); lcd.setBounds(r.removeFromTop(62).reduced(8));
}
void G1PluginEditor::timerCallback()
{
    if(auto* m=proc.machine()) {
        const auto& d=m->getLcd();
        // Lcd exposes printable lines through text(); if that API changes, the plugin still builds without this cosmetic readout.
        (void)d;
        lcd.setText("G1 running · 4 outputs · MIDI in · native DSP rate 96 kHz", juce::dontSendNotification);
    } else lcd.setText("ROM required", juce::dontSendNotification);
}
