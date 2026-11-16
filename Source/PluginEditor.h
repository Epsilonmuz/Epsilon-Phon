#pragma once
#include "PluginProcessor.h"
#include "../Shared/epsilon_ui/EpsilonUI.h"

struct PhonView
{
    EpsilonPhonProcessor::Snapshot snap;
    phon::Profile target;
    phon::Comparison cmp;
    bool earView = false;
    float listen = 79.0f;
    std::array<float, 64> matchResp {};      // match EQ response sampled across the graph
    bool matchOn = false;
};

// The hero: your mix's tonal balance against the hits, 25 Hz - 16 kHz.
// "Ear X-ray" re-draws everything through ISO 226 at your listening level - what your ears actually hear.
class BalanceGraph : public juce::Component
{
public:
    void setView(const PhonView& v, bool targetChanged);
    void paint(juce::Graphics&) override;
    void resized() override { cache = {}; }
    juce::Rectangle<float> plotArea() const;
private:
    PhonView view;
    juce::Image cache;
    float yTop = 12, yBottom = -30;
    void renderCache();
    float xFor(double hz, juce::Rectangle<float> r) const;
    float yFor(float db, juce::Rectangle<float> r) const;
    float shown(int band, float db) const;
};

// Real time: every third-octave band against the target, right now. Up (amber/red) = louder than it
// should be, down (blue) = missing. Peak-hold ticks, and the worst offenders named in plain words.
class LiveMeter : public juce::Component
{
public:
    void update(const phon::LiveDev& d, float dt, bool haveTarget);
    void paint(juce::Graphics&) override;
private:
    phon::LiveTracker tracker;
    bool haveTarget = false;
    juce::String shownOver, shownUnder;
    juce::uint32 lastText = 0;
    std::array<phon::Offender, 3> over {}, under {};
    int nOver = 0, nUnder = 0;
};

// 3 listening levels x 5 ranges: how each range SOUNDS against the target at that volume (sones).
class TranslationGrid : public juce::Component
{
public:
    void setView(const PhonView& v) { view = v; repaint(); }
    void paint(juce::Graphics&) override;
private:
    PhonView view;
};

class LoudnessView : public juce::Component
{
public:
    void set(const phon::LoudnessState& l, float target, const juce::String& targetName);
    void paint(juce::Graphics&) override;
private:
    phon::LoudnessState loud;
    float target = -9.0f;
    juce::String targetName;
};

class NotesView : public juce::Component
{
public:
    void set(const std::vector<std::string>& notes, const juce::String& status);
    void paint(juce::Graphics&) override;
private:
    std::vector<std::string> lines;
    juce::String status;
};

class EpsilonPhonEditor : public juce::AudioProcessorEditor, public juce::FileDragAndDropTarget, private juce::Timer
{
public:
    explicit EpsilonPhonEditor(EpsilonPhonProcessor&);
    ~EpsilonPhonEditor() override;
    void paint(juce::Graphics&) override;
    void resized() override;
    bool isInterestedInFileDrag(const juce::StringArray&) override { return true; }
    void filesDropped(const juce::StringArray&, int, int) override;
    void fileDragEnter(const juce::StringArray&, int, int) override { dragOver = true; repaint(); }
    void fileDragExit(const juce::StringArray&) override { dragOver = false; repaint(); }
    void refreshNow() { timerCallback(); }

private:
    EpsilonPhonProcessor& proc;
    epsilon_ui::LookAndFeel look { epsilon_ui::Tier::Epsilon };
    epsilon_ui::HeaderBar header { epsilon_ui::Tier::Epsilon, "Phon", "Equal-loudness mix referee" };
    epsilon_ui::FooterBar footer { epsilon_ui::Tier::Epsilon, JucePlugin_VersionString };
    juce::TooltipWindow tooltips { this, 700 };

    juce::TextButton loadButton { "Load hits" }, resetButton { "Reset" }, learnButton { "Learn match" }, clearButton { "Clear" };
    juce::TextButton xrayButton { "Ear X-ray" };
    juce::ComboBox targetBox, loudBox;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> targetAtt, loudAtt;
    epsilon_ui::Knob listenKnob { proc.state, "listen", "Listening level", true };
    epsilon_ui::Knob matchKnob { proc.state, "match", "Match" };
    epsilon_ui::Knob outputKnob { proc.state, "output", "Output" };
    epsilon_ui::ModeSwitch hearAt { proc.state, "hearAt" };

    BalanceGraph graph;
    LiveMeter live;
    TranslationGrid grid;
    LoudnessView loudness;
    NotesView notes;
    std::unique_ptr<juce::FileChooser> chooser;

    juce::Rectangle<int> livePanel, heroPanel, loudPanel, gridPanel, notesPanel, ctrlPanel, hearLabel;
    juce::Image chrome;
    int lastSerial = -1, lastRefSerial = -1, lastTarget = -1, lastHear = -1, lastMatchSerial = -1;
    float lastListen = -1, lastMatch = -1;
    bool lastXray = false, dragOver = false;
    juce::uint32 lastNotes = 0;
    double lastLiveMs = 0;
    int lastLiveSerial = -1, staleTicks = 0;

    void timerCallback() override;
    void chooseFiles();
    juce::String statusText() const;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(EpsilonPhonEditor)
};
