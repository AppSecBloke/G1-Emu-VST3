#include "PluginEditor.h"
#include "format/ValueFormatters.h"
#include <algorithm>
#include <limits>

namespace
{
    using View = G1PluginProcessor::ParameterView;

    class ParameterControl final : public juce::Component
    {
    public:
        ParameterControl(G1PluginProcessor& p, uint64_t g, const View& v)
            : processor(p), generation(g), view(v)
        {
            name.setText(view.name, juce::dontSendNotification);
            name.setJustificationType(juce::Justification::centred);
            valueLabel.setJustificationType(juce::Justification::centred);
            for (auto* label : {&name, &valueLabel})
            {
                label->setColour(juce::Label::textColourId, juce::Colours::black);
                addAndMakeVisible(*label);
            }
            const int span = view.maximum - view.minimum;
            if (span == 1)
            {
                addAndMakeVisible(toggle);
                toggle.setColour(juce::ToggleButton::textColourId, juce::Colours::black);
                toggle.setColour(juce::ToggleButton::tickColourId, juce::Colour(0xffa52020));
                toggle.onClick = [this] { edit(toggle.getToggleState() ? view.maximum : view.minimum); };
            }
            else
            {
                // Formatter labels must be distinct and contain information
                // beyond the raw number before we infer a small choice control.
                juce::StringArray labels;
                bool useful = false, distinct = true;
                if (span <= 7 && view.formatter.isNotEmpty())
                    for (int raw = view.minimum; raw <= view.maximum; ++raw)
                    {
                        const auto label = ValueFormatters::format(view.formatter, raw);
                        useful = useful || label != juce::String(raw);
                        distinct = distinct && label.isNotEmpty() && !labels.contains(label);
                        labels.add(label);
                    }
                if (labels.size() == span + 1 && useful && distinct)
                {
                    for (int i = 0; i < labels.size(); ++i)
                        choice.addItem(labels[i], i + 1);
                    addAndMakeVisible(choice);
                    choice.onChange = [this]
                    {
                        if (choice.getSelectedId() > 0)
                            edit(view.minimum + choice.getSelectedId() - 1);
                    };
                }
                else
                {
                    slider.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
                    slider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 64, 22);
                    slider.setRange(view.minimum, view.maximum, 1.0);
                    slider.setNumDecimalPlacesToDisplay(0);
                    slider.setColour(juce::Slider::rotarySliderFillColourId, juce::Colour(0xffa52020));
                    slider.setColour(juce::Slider::textBoxTextColourId, juce::Colours::black);
                    slider.setColour(juce::Slider::textBoxBackgroundColourId, juce::Colours::white);
                    addAndMakeVisible(slider);
                    slider.onValueChange = [this] { edit(juce::roundToInt(slider.getValue())); };
                }
            }
            slider.setName(view.name);
            choice.setName(view.name);
            toggle.setName(view.name);
            showValue(view.value); // Never notifies listeners or queues an edit.
        }

        void update(const View& updated)
        {
            if (!slider.isMouseButtonDown())
                showValue(updated.value);
        }

        void resized() override
        {
            auto bounds = getLocalBounds().reduced(4);
            name.setBounds(bounds.removeFromTop(24));
            valueLabel.setBounds(bounds.removeFromBottom(22));
            slider.setBounds(bounds);
            choice.setBounds(bounds.withSizeKeepingCentre(bounds.getWidth() - 8, 28));
            toggle.setBounds(bounds.withSizeKeepingCentre(bounds.getWidth() - 8, 28));
        }

    private:
        void showValue(int value)
        {
            view.value = value;
            slider.setValue(value, juce::dontSendNotification);
            choice.setSelectedId(value - view.minimum + 1, juce::dontSendNotification);
            toggle.setToggleState(value == view.maximum, juce::dontSendNotification);
            const auto formatted = ValueFormatters::format(view.formatter, value);
            toggle.setButtonText(formatted);
            valueLabel.setText(formatted == juce::String(value) ? formatted
                              : formatted + " [" + juce::String(value) + "]",
                               juce::dontSendNotification);
        }
        void edit(int value)
        {
            if (processor.editParameter(generation, view.address, value))
                showValue(juce::jlimit(view.minimum, view.maximum, value));
        }
        G1PluginProcessor& processor;
        uint64_t generation;
        View view;
        juce::Label name, valueLabel;
        juce::Slider slider;
        juce::ToggleButton toggle;
        juce::ComboBox choice;
    };
}

class G1PluginEditor::ParameterPanel final : public juce::Component
{
public:
    explicit ParameterPanel(G1PluginProcessor& p) : processor(p)
    {
        empty.setText("Load a patch to discover its ordinary parameters.", juce::dontSendNotification);
        empty.setColour(juce::Label::textColourId, juce::Colours::black);
        addAndMakeVisible(empty);
    }

    void update(const G1PluginProcessor::ParameterSnapshot& snapshot)
    {
        if (snapshot.generation != generation)
        {
            groups.clear();
            generation = snapshot.generation;
            for (const auto& view : snapshot.parameters)
            {
                auto group = std::find_if(groups.begin(), groups.end(), [&](const auto& item)
                { return item->section == view.address.section && item->module == view.address.module; });
                if (group == groups.end())
                {
                    auto item = std::make_unique<Group>();
                    item->section = view.address.section;
                    item->module = view.address.module;
                    item->title.setText((view.address.section == 1 ? "Poly / " : "Common / ")
                                        + view.moduleTitle + " (" + view.moduleTypeName + ")",
                                        juce::dontSendNotification);
                    item->title.setFont(juce::FontOptions(17.0f, juce::Font::bold));
                    item->title.setColour(juce::Label::textColourId, juce::Colours::black);
                    addAndMakeVisible(item->title);
                    groups.push_back(std::move(item));
                    group = groups.end() - 1;
                }
                auto control = std::make_unique<ParameterControl>(processor, generation, view);
                addAndMakeVisible(*control);
                (*group)->controls.push_back(std::move(control));
            }
            empty.setVisible(groups.empty());
            layout(getWidth());
        }
        size_t index = 0;
        for (auto& group : groups)
            for (auto& control : group->controls)
                if (index < snapshot.parameters.size())
                    control->update(snapshot.parameters[index++]);
    }

    void layout(int width)
    {
        width = std::max(160, width);
        const int columns = std::max(1, width / 170);
        const int cellWidth = width / columns;
        int y = 0;
        empty.setBounds(8, 8, width - 16, 40);
        for (auto& group : groups)
        {
            group->title.setBounds(8, y, width - 16, 30);
            y += 30;
            for (size_t i = 0; i < group->controls.size(); ++i)
                group->controls[i]->setBounds(static_cast<int>(i) % columns * cellWidth,
                                             y + static_cast<int>(i) / columns * 154,
                                             cellWidth, 154);
            y += ((static_cast<int>(group->controls.size()) + columns - 1) / columns) * 154 + 12;
        }
        setSize(width, std::max(60, y));
    }

private:
    struct Group
    {
        int section = 0, module = 0;
        juce::Label title;
        std::vector<std::unique_ptr<ParameterControl>> controls;
    };
    G1PluginProcessor& processor;
    uint64_t generation = std::numeric_limits<uint64_t>::max();
    juce::Label empty;
    std::vector<std::unique_ptr<Group>> groups;
};

G1PluginEditor::G1PluginEditor(G1PluginProcessor& p) : AudioProcessorEditor(&p), proc(p)
{
    title.setText("G1-Emu - Nord Modular G1 emulator - playability build #8 - CMPM fix", juce::dontSendNotification);
    title.setFont(juce::FontOptions(20.0f, juce::Font::bold));
    diag.setFont(juce::FontOptions(14.0f));
    diag.setJustificationType(juce::Justification::topLeft);
    for (auto* label : {&title, &rom, &patch, &status, &diag})
    {
        label->setColour(juce::Label::textColourId, juce::Colours::black);
        addAndMakeVisible(*label);
    }
    for (auto* button : {&chooseRom, &loadPatch, &panicButton})
        addAndMakeVisible(*button);
    parameterPanel = std::make_unique<ParameterPanel>(proc);
    parameterViewport.setViewedComponent(parameterPanel.get(), false);
    parameterViewport.setScrollBarsShown(true, false);
    addAndMakeVisible(parameterViewport);

    chooseRom.onClick = [this]
    {
        chooser = std::make_unique<juce::FileChooser>("Choose your Nord Modular G1 512 KB ROM", juce::File{}, "*");
        chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
            [self = juce::Component::SafePointer<G1PluginEditor>(this)](const juce::FileChooser& fc)
            {
                if (!self || fc.getResult() == juce::File{}) return;
                juce::String error;
                if (!self->proc.loadRom(fc.getResult(), error))
                    self->status.setText(error, juce::dontSendNotification);
                self->refresh();
            });
    };
    loadPatch.onClick = [this]
    {
        chooser = std::make_unique<juce::FileChooser>("Load Nord Modular patch", juce::File{}, "*.pch");
        chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
            [self = juce::Component::SafePointer<G1PluginEditor>(this)](const juce::FileChooser& fc)
            {
                if (!self || fc.getResult() == juce::File{}) return;
                juce::String error;
                self->proc.loadPatch(fc.getResult(), error);
                self->refresh();
            });
    };
    panicButton.onClick = [this] { proc.panic(); refresh(); };
    setSize(980, 860);
    refresh();
    startTimerHz(8);
}

G1PluginEditor::~G1PluginEditor()
{
    stopTimer();
    parameterViewport.setViewedComponent(nullptr, false);
}

void G1PluginEditor::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colour(0xffd9d9d5));
    g.setColour(juce::Colours::black);
    g.drawRect(getLocalBounds(), 2);
}

void G1PluginEditor::resized()
{
    auto bounds = getLocalBounds().reduced(18);
    title.setBounds(bounds.removeFromTop(32));
    auto buttons = bounds.removeFromTop(36);
    chooseRom.setBounds(buttons.removeFromLeft(130));
    buttons.removeFromLeft(10);
    loadPatch.setBounds(buttons.removeFromLeft(130));
    buttons.removeFromLeft(10);
    panicButton.setBounds(buttons.removeFromLeft(100));
    rom.setBounds(bounds.removeFromTop(25));
    patch.setBounds(bounds.removeFromTop(25));
    status.setBounds(bounds.removeFromTop(30));
    diag.setBounds(bounds.removeFromBottom(185));
    bounds.removeFromBottom(10);
    parameterViewport.setBounds(bounds);
    if (parameterPanel)
        parameterPanel->layout(bounds.getWidth() - parameterViewport.getScrollBarThickness());
}

void G1PluginEditor::refresh()
{
    parameterPanel->update(proc.parameterSnapshot());
    const auto romPath = proc.romPath(), patchPath = proc.patchPath();
    rom.setText("ROM: " + (romPath.isEmpty() ? juce::String("not selected") : romPath), juce::dontSendNotification);
    patch.setText("Patch: " + (patchPath.isEmpty() ? juce::String("none") : patchPath), juce::dontSendNotification);
    status.setText(proc.status(), juce::dontSendNotification);
    diag.setText(proc.diagnostics(), juce::dontSendNotification);
}

void G1PluginEditor::timerCallback() { refresh(); }
