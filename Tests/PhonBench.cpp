// Epsilon Phon host bench: drives the real processor like a DAW would.
//  - writes two synthetic "hit" records, loads them as the reference (averaged)
//  - plays a mix with a heavy low-mid for 20 s at 44.1 / 48 kHz; checks analysis + loudness
//  - Learn match: the match EQ cuts the low-mid; output stays finite
//  - Hear at Phone: 40 Hz drops, 1 kHz stays
//  - state round-trip keeps the reference and match curve
//  - real-time cost per block; editor snapshots to docs/
//   PhonBench [snapshot-dir]
#include <JuceHeader.h>
#include "PluginProcessor.h"
#include "PluginEditor.h"
#include <random>

namespace
{
int fails = 0;
void check(bool ok, const juce::String& what) { std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", what.toRawUTF8()); if (! ok) ++fails; }

struct Pink
{
    std::mt19937 rng { 11 }; std::normal_distribution<float> nd { 0.0f, 1.0f };
    double b0 = 0, b1 = 0, b2 = 0, b3 = 0, b4 = 0, b5 = 0, b6 = 0;
    float next()
    {
        const double w = nd(rng);
        b0 = 0.99886 * b0 + w * 0.0555179; b1 = 0.99332 * b1 + w * 0.0750759; b2 = 0.96900 * b2 + w * 0.1538520;
        b3 = 0.86650 * b3 + w * 0.3104856; b4 = 0.55000 * b4 + w * 0.5329522; b5 = -0.7616 * b5 - w * 0.0168980;
        const double o = (b0 + b1 + b2 + b3 + b4 + b5 + b6 + w * 0.5362) * 0.05; b6 = w * 0.115926;
        return (float) o;
    }
};

juce::File writeHit(int idx, double sr)
{
    const int len = (int) (sr * 15);
    juce::AudioBuffer<float> b(2, len);
    Pink p; p.rng.seed((unsigned) (100 + idx));
    for (int i = 0; i < len; ++i)
    {
        const float sub = 0.25f * (float) std::sin(2 * juce::MathConstants<double>::pi * 50.0 * i / sr);
        const float v = 0.9f * p.next() + sub;
        b.setSample(0, i, v); b.setSample(1, i, v);
    }
    auto f = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("phon-hit-" + juce::String(idx) + ".wav");
    f.deleteFile();
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::AudioFormatWriter> w(wav.createWriterFor(f.createOutputStream().release(), sr, 2, 24, {}, 0));
    w->writeFromAudioSampleBuffer(b, 0, len);
    return f;
}

void setParam(EpsilonPhonProcessor& p, const juce::String& id, float value)
{
    auto* prm = p.state.getParameter(id);
    prm->setValueNotifyingHost(prm->convertTo0to1(value));
}

// Plays `seconds` of signal through the processor. gen(i) returns the mono sample.
template <typename Gen>
double play(EpsilonPhonProcessor& p, double sr, double seconds, Gen&& gen, juce::AudioBuffer<float>* lastOut = nullptr)
{
    const int block = 512;
    juce::AudioBuffer<float> buf(2, block);
    juce::MidiBuffer midi;
    const int total = (int) (sr * seconds);
    double worst = 0;
    static long long n = 0;                      // continuous phase across calls (no clicks between plays)
    for (int pos = 0; pos < total; pos += block)
    {
        for (int i = 0; i < block; ++i) { const float v = gen(n++); buf.setSample(0, i, v); buf.setSample(1, i, v); }
        const auto t0 = juce::Time::getHighResolutionTicks();
        p.processBlock(buf, midi);
        worst = std::max(worst, juce::Time::highResolutionTicksToSeconds(juce::Time::getHighResolutionTicks() - t0));
        // The bench runs faster than real time: pace it so the analysis worker (25 ms cycle) keeps up
        // on any machine instead of overflowing its ring.
        if ((pos / block) % 16 == 15) juce::MessageManager::getInstance()->runDispatchLoopUntil(30);
    }
    if (lastOut) lastOut->makeCopyOf(buf);
    juce::Thread::sleep(200);
    return worst / (block / sr) * 100.0;
}

float rmsDb(const juce::AudioBuffer<float>& b) { return juce::Decibels::gainToDecibels(b.getRMSLevel(0, 0, b.getNumSamples()), -150.0f); }
}

int main(int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI gui;
    const juce::File snapDir = argc > 1 ? juce::File::getCurrentWorkingDirectory().getChildFile(argv[1]) : juce::File::getCurrentWorkingDirectory().getChildFile("docs");
    snapDir.createDirectory();

    for (double sr : { 44100.0, 48000.0 })
    {
        const juce::String at = " @" + juce::String((int) sr);
        EpsilonPhonProcessor p;
        p.setPlayConfigDetails(2, 2, sr, 512);
        p.prepareToPlay(sr, 512);

        // reference records
        juce::Array<juce::File> hits { writeHit(1, sr), writeHit(2, sr) };
        p.loadReferences(hits);
        for (int i = 0; i < 600 && p.isLoadingReference(); ++i) juce::MessageManager::getInstance()->runDispatchLoopUntil(10);
        const auto ref = p.getReference();
        check(ref.valid && ref.count == 2, "two hit records analysed and averaged" + at);
        check(ref.lufs < -5 && ref.lufs > -30, "reference loudness measured: " + juce::String(ref.lufs, 1) + " LUFS" + at);
        setParam(p, "target", 0.0f);
        check(p.currentTarget().valid, "target = loaded hits" + at);

        // mix: same pink + sub, plus a strong 250 Hz tone (heavy low-mid)
        Pink pk;
        const double w250 = 2 * juce::MathConstants<double>::pi * 250.0 / sr, w50 = 2 * juce::MathConstants<double>::pi * 50.0 / sr;
        auto mix = [&](long long i) { return 0.9f * pk.next() + 0.25f * (float) std::sin(w50 * i) + 0.35f * (float) std::sin(w250 * i); };
        const double cpuPct = play(p, sr, 20.0, mix);
        auto s = p.getSnapshot();
        check(s.seconds > 15.0, "analysis worker averaged " + juce::String(s.seconds, 1) + " s" + at);
        check(s.loud.hasIntegrated && std::isfinite(s.loud.integrated), "integrated loudness " + juce::String(s.loud.integrated, 1) + " LUFS" + at);
        const auto cmp = phon::compare(s.average, p.currentTarget(), 79.0f);
        check(cmp.valid && cmp.rangeDb[2] > 1.0f, "low-mid flagged heavy vs hits: +" + juce::String(cmp.rangeDb[2], 1) + " dB" + at);
        check(std::abs(cmp.rangeDb[0]) < 1.5f, "sub matches the hits: " + juce::String(cmp.rangeDb[0], 1) + " dB" + at);
        check(cpuPct < 50.0, "worst block " + juce::String(cpuPct, 1) + " % of real-time budget (debug-safe bound)" + at);

        // real time: the live view names 250 Hz as too loud while it plays
        {
            const auto ld = phon::liveDeviation(s.live, p.currentTarget());
            phon::Offender off[3]; const int n = phon::offenders(ld.dev, 2.5f, true, off, 3);
            check(s.liveActive && ld.valid && n >= 1 && off[0].band == 10, "live view: 250 Hz named too loud right now: +" + juce::String(n ? off[0].db : 0.0f, 1) + " dB" + at);
            // a new problem (3 kHz tone) appears: shown within 0.3 s of audio
            const double w3k = 2 * juce::MathConstants<double>::pi * 3162.0 / sr;
            play(p, sr, 0.3, [&](long long i) { return mix(i) + 0.2f * (float) std::sin(w3k * i); });
            const auto ld2 = phon::liveDeviation(p.getSnapshot().live, p.currentTarget());
            check(ld2.dev[21] > 3.0f, "live view: a new 3.2 kHz problem shows within 0.3 s: +" + juce::String(ld2.dev[21], 1) + " dB" + at);
            play(p, sr, 0.5, [](long long) { return 0.0f; });
            check(! p.getSnapshot().liveActive, "live view: gated 0.5 s after the audio stops" + at);
            p.resetAnalysis();
            play(p, sr, 20.0, mix);
            s = p.getSnapshot();
        }

        // learn match
        p.learnMatch();
        const auto mc = p.getMatchCurve();
        check(p.hasMatch() && mc[3] < -1.0, "Learn match cuts 250 Hz: " + juce::String(mc[3], 1) + " dB" + at);
        p.resetAnalysis();
        juce::AudioBuffer<float> out;
        play(p, sr, 12.0, mix, &out);
        s = p.getSnapshot();
        const auto cmp2 = phon::compare(s.average, p.currentTarget(), 79.0f);
        check(cmp2.valid && cmp2.rangeDb[2] < cmp.rangeDb[2] - 0.5f, "after match the low-mid gap shrinks: " + juce::String(cmp2.rangeDb[2], 1) + " dB" + at);
        check(cmp2.score > cmp.score, "hit-match score improves " + juce::String(cmp.score, 0) + " -> " + juce::String(cmp2.score, 0) + at);
        bool finite = true; for (int i = 0; i < out.getNumSamples(); ++i) finite &= std::isfinite(out.getSample(0, i));
        check(finite, "output finite" + at);

        // Hear at phone: 40 Hz quieter, 1 kHz unchanged
        setParam(p, "match", 0.0f);
        const double w40 = 2 * juce::MathConstants<double>::pi * 40.0 / sr, w1k = 2 * juce::MathConstants<double>::pi * 1000.0 / sr;
        juce::AudioBuffer<float> o40, o1k, s40, s1k;
        play(p, sr, 1.0, [&](long long i) { return 0.25f * (float) std::sin(w40 * i); }, &o40);
        play(p, sr, 1.0, [&](long long i) { return 0.25f * (float) std::sin(w1k * i); }, &o1k);
        setParam(p, "hearAt", 1.0f);
        play(p, sr, 1.0, [&](long long i) { return 0.25f * (float) std::sin(w40 * i); }, &s40);
        play(p, sr, 1.0, [&](long long i) { return 0.25f * (float) std::sin(w1k * i); }, &s1k);
        const float d40 = rmsDb(s40) - rmsDb(o40), d1k = rmsDb(s1k) - rmsDb(o1k);
        check(d40 < -6.0f, "Hear at Phone: 40 Hz drops " + juce::String(d40, 1) + " dB" + at);
        check(std::abs(d1k) < 0.7f, "Hear at Phone: 1 kHz holds " + juce::String(d1k, 2) + " dB" + at);
        setParam(p, "hearAt", 0.0f);

        // state round trip
        setParam(p, "listen", 83.0f);
        juce::MemoryBlock mb;
        p.getStateInformation(mb);
        EpsilonPhonProcessor q;
        q.setStateInformation(mb.getData(), (int) mb.getSize());
        const auto r2 = q.getReference();
        check(r2.valid && r2.name == ref.name && std::abs(r2.db[10] - ref.db[10]) < 0.02f, "state keeps the hit reference" + at);
        check(q.hasMatch() && std::abs(q.getMatchCurve()[3] - mc[3]) < 0.01, "state keeps the match curve" + at);
        check(std::abs(q.state.getRawParameterValue("listen")->load() - 83.0f) < 0.01f, "state keeps parameters" + at);

        if (sr == 48000.0)
        {
            setParam(p, "match", 100.0f);
            p.resetAnalysis();
            play(p, sr, 10.0, mix);
            for (int pass = 0; pass < 3; ++pass)
            {
                p.state.state.setProperty("earXray", pass == 1, nullptr);
                std::unique_ptr<juce::AudioProcessorEditor> ed(p.createEditor());
                auto* pe = dynamic_cast<EpsilonPhonEditor*>(ed.get());
                ed->setSize(1080, 780);
                for (int i = 0; i < 12; ++i) { play(p, sr, 0.1, mix); juce::MessageManager::getInstance()->runDispatchLoopUntil(20); if (pe) pe->refreshNow(); }
                if (pe && pass == 2) pe->showHelp(true);
                const auto t0 = juce::Time::getMillisecondCounterHiRes();
                auto img = ed->createComponentSnapshot(ed->getLocalBounds(), true, 2.0f);
                const auto ms = juce::Time::getMillisecondCounterHiRes() - t0;
                auto file = snapDir.getChildFile(pass == 0 ? "epsilon-phon-editor.png" : pass == 1 ? "epsilon-phon-ear-xray.png" : "epsilon-phon-help.png");
                file.deleteFile();
                juce::PNGImageFormat png;
                if (auto os = file.createOutputStream()) png.writeImageToStream(img, *os);
                check(file.existsAsFile(), "editor snapshot " + file.getFileName() + " (" + juce::String(ms, 1) + " ms full paint @2x)");
                p.state.state.setProperty("earXray", false, nullptr);
            }
        }
    }
    std::printf("\n%s (%d failure%s)\n", fails ? "FAILED" : "ALL PASSED", fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
