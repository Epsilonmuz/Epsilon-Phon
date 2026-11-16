#pragma once
#include <JuceHeader.h>
#include "PhonEngine.h"

// Signal path (zero latency):
//   in -> Match EQ -> Output gain -> [meter + analyser tap = what you'll bounce] -> Hear-at EQ (monitor only) -> out
class EpsilonPhonProcessor : public juce::AudioProcessor
{
public:
    EpsilonPhonProcessor();
    ~EpsilonPhonProcessor() override;

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported(const BusesLayout&) const override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }
    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}
    void getStateInformation(juce::MemoryBlock&) override;
    void setStateInformation(const void*, int) override;

    juce::AudioProcessorValueTreeState state;
    static juce::StringArray targetChoices();
    static juce::StringArray hearAtChoices();
    static float hearAtLevel(int index);                  // dB SPL the "Hear at" choice simulates (0 = off)

    // ---- analysis snapshot (worker thread -> UI)
    struct Snapshot
    {
        phon::Bands fast {}, average {}, live {};    // live: fastest view, for the real-time meter
        double seconds = 0.0;
        bool liveActive = false;
        phon::LoudnessState loud;
        int serial = 0;
    };
    Snapshot getSnapshot() const;
    void resetAnalysis();                                  // average + integrated loudness

    // ---- target
    phon::Profile currentTarget() const;                   // loaded hits or modelled genre
    float loudnessTarget() const;
    phon::Profile getReference() const { const juce::ScopedLock sl(refLock); return reference; }
    void loadReferences(const juce::Array<juce::File>& files);   // async; averages every record dropped
    bool isLoadingReference() const { return loadingRefs.load(); }
    juce::String getReferenceMessage() const { const juce::ScopedLock sl(refLock); return refMessage; }
    int getReferenceSerial() const { return refSerial.load(); }

    // ---- match
    void learnMatch();                                     // snapshot the correction from the current average
    void clearMatch();
    std::array<double, phon::OctaveEq::numBandsEq> getMatchCurve() const;
    bool hasMatch() const { return matchLearned.load(); }
    double matchResponseDb(double hz) const;               // what the match EQ is doing right now (for drawing)

    float getCpuPercent() const { return cpu.load(); }
    double getSampleRateSafe() const { return sr.load(); }

private:
    std::atomic<double> sr { 48000.0 };
    phon::OctaveEq matchEq, simEq;
    phon::LoudnessMeter meter;
    phon::Ring ring;
    std::vector<float> mono;
    juce::LinearSmoothedValue<float> outGain;

    // match / sim curves: written on the message thread, read on the audio thread
    std::array<std::atomic<float>, phon::OctaveEq::numBandsEq> matchWant;
    std::atomic<bool> matchLearned { false };
    std::atomic<int> matchSerial { 0 };
    int seenMatchSerial = -1;
    float seenAmount = -1.0f, seenListen = -1.0f; int seenHearAt = -1;
    std::array<double, phon::OctaveEq::numBandsEq> solvedMatch {};

    std::atomic<bool> resetRequest { false };
    juce::SpinLock loudLock;
    phon::LoudnessState loudShared;
    int loudCountdown = 0;

    // worker
    class Worker;
    std::unique_ptr<Worker> worker;
    mutable juce::CriticalSection snapLock;
    Snapshot snap;

    // reference
    mutable juce::CriticalSection refLock;
    phon::Profile reference;
    juce::String refMessage;
    std::atomic<bool> loadingRefs { false };
    std::atomic<int> refSerial { 0 };
    std::unique_ptr<juce::Thread> refThread;

    std::atomic<float> cpu { 0.0f };
    std::atomic<float>* pTarget = nullptr; std::atomic<float>* pListen = nullptr; std::atomic<float>* pHearAt = nullptr;
    std::atomic<float>* pMatch = nullptr; std::atomic<float>* pOutput = nullptr; std::atomic<float>* pLoudTarget = nullptr;

    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(EpsilonPhonProcessor)
};
