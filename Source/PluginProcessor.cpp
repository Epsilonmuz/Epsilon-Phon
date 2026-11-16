#include "PluginProcessor.h"
#include "PluginEditor.h"

namespace
{
const juce::String idTarget = "target", idListen = "listen", idHearAt = "hearAt", idMatch = "match", idOutput = "output", idLoudTarget = "loudTarget";
}

//==============================================================================
// Analysis worker: drains the audio ring into the FFT spectrum and publishes snapshots (~30 Hz).
class EpsilonPhonProcessor::Worker : public juce::Thread
{
public:
    explicit Worker(EpsilonPhonProcessor& p) : juce::Thread("Epsilon Phon analysis"), owner(p) {}
    void prepare(double sampleRate) { const juce::ScopedLock sl(lock); spec.prepare(sampleRate, 14); quick.prepare(sampleRate, 12); rate = sampleRate; level = 0; pending = true; }
    void requestReset() { resetFlag = true; }
    void run() override
    {
        std::vector<float> buf(4096);
        while (! threadShouldExit())
        {
            {
                const juce::ScopedLock sl(lock);
                if (resetFlag.exchange(false)) { spec.reset(); quick.reset(); }
                int got = 0, total = 0;
                while ((got = owner.ring.pop(buf.data(), (int) buf.size())) > 0 && total < 1 << 18) {
                    spec.push(buf.data(), got); quick.push(buf.data(), got); total += got;
                    double e = 0; for (int i = 0; i < got; ++i) e += (double) buf[(size_t) i] * buf[(size_t) i];
                    const double k = std::exp(-got / (0.03 * rate));          // ~30 ms input level, for the silence gate
                    level = k * level + (1.0 - k) * e / got;
                }
                if (total > 0 || pending)
                {
                    pending = false;
                    Snapshot s;
                    s.fast = spec.fastDb();
                    s.average = spec.averageDb();
                    s.liveActive = level > 1e-6;                              // above -60 dBFS RMS right now
                    // real-time view: 4096-point analyser above 200 Hz, the long one below (it needs the resolution)
                    const auto lo = spec.quickDb(), hi = quick.quickDb();
                    for (int b = 0; b < phon::numBands; ++b) s.live[(size_t) b] = phon::bandCentre(b) < 200.0 ? lo[(size_t) b] : hi[(size_t) b];
                    s.seconds = spec.secondsAveraged();
                    {
                        const juce::SpinLock::ScopedLockType ll(owner.loudLock);
                        s.loud = owner.loudShared;
                    }
                    const juce::ScopedLock sl2(owner.snapLock);
                    s.serial = owner.snap.serial + 1;
                    owner.snap = s;
                }
            }
            wait(25);
        }
    }
private:
    EpsilonPhonProcessor& owner;
    juce::CriticalSection lock;
    phon::Spectrum spec, quick;
    double rate = 48000.0, level = 0.0;
    std::atomic<bool> resetFlag { false };
    bool pending = true;
};

//==============================================================================
juce::StringArray EpsilonPhonProcessor::targetChoices()
{
    juce::StringArray s { "Hit references" };
    for (int g = 0; g < (int) phon::Genre::count; ++g) s.add(phon::genreName((phon::Genre) g));
    return s;
}
juce::StringArray EpsilonPhonProcessor::hearAtChoices() { return { "Off", "Phone", "Quiet", "Loud" }; }
float EpsilonPhonProcessor::hearAtLevel(int i)
{
    static const float l[] = { 0.0f, 55.0f, 65.0f, 90.0f };
    return l[juce::jlimit(0, 3, i)];
}

juce::AudioProcessorValueTreeState::ParameterLayout EpsilonPhonProcessor::createLayout()
{
    using namespace juce;
    AudioProcessorValueTreeState::ParameterLayout l;
    l.add(std::make_unique<AudioParameterChoice>(ParameterID { idTarget, 1 }, "Target", targetChoices(), 1));
    l.add(std::make_unique<AudioParameterFloat>(ParameterID { idListen, 1 }, "Listening level", NormalisableRange<float>(60.0f, 90.0f, 0.5f), 79.0f,
                                                AudioParameterFloatAttributes().withLabel("dB SPL")
                                                    .withStringFromValueFunction([](float v, int) { return String(v, 1) + " dB"; })));
    l.add(std::make_unique<AudioParameterChoice>(ParameterID { idHearAt, 1 }, "Hear at", hearAtChoices(), 0));
    l.add(std::make_unique<AudioParameterFloat>(ParameterID { idMatch, 1 }, "Match", NormalisableRange<float>(0.0f, 100.0f, 1.0f), 0.0f,
                                                AudioParameterFloatAttributes().withLabel("%")
                                                    .withStringFromValueFunction([](float v, int) { return String((int) std::round(v)) + " %"; })));
    l.add(std::make_unique<AudioParameterFloat>(ParameterID { idOutput, 1 }, "Output", NormalisableRange<float>(-12.0f, 12.0f, 0.1f), 0.0f,
                                                AudioParameterFloatAttributes().withLabel("dB")
                                                    .withStringFromValueFunction([](float v, int) { return String(v, 1) + " dB"; })));
    l.add(std::make_unique<AudioParameterChoice>(ParameterID { idLoudTarget, 1 }, "Loudness target",
                                                 StringArray { "Match target", "Streaming -14", "Apple -16", "Club -7" }, 0));
    return l;
}

EpsilonPhonProcessor::EpsilonPhonProcessor()
    : AudioProcessor(BusesProperties().withInput("Input", juce::AudioChannelSet::stereo(), true)
                                      .withOutput("Output", juce::AudioChannelSet::stereo(), true)),
      state(*this, nullptr, "EpsilonPhon", createLayout())
{
    for (auto& w : matchWant) w.store(0.0f);
    pTarget = state.getRawParameterValue(idTarget);
    pListen = state.getRawParameterValue(idListen);
    pHearAt = state.getRawParameterValue(idHearAt);
    pMatch = state.getRawParameterValue(idMatch);
    pOutput = state.getRawParameterValue(idOutput);
    pLoudTarget = state.getRawParameterValue(idLoudTarget);
    ring.prepare(1 << 18);
    worker = std::make_unique<Worker>(*this);
    worker->prepare(48000.0);
    worker->startThread(juce::Thread::Priority::low);
}

EpsilonPhonProcessor::~EpsilonPhonProcessor()
{
    if (refThread) refThread->stopThread(4000);
    worker->stopThread(2000);
}

bool EpsilonPhonProcessor::isBusesLayoutSupported(const BusesLayout& l) const
{
    const auto out = l.getMainOutputChannelSet();
    if (out != juce::AudioChannelSet::stereo() && out != juce::AudioChannelSet::mono()) return false;
    return l.getMainInputChannelSet() == out;
}

void EpsilonPhonProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    sr.store(sampleRate);
    const int ch = juce::jmax(1, getTotalNumOutputChannels());
    matchEq.prepare(sampleRate, ch);
    simEq.prepare(sampleRate, ch);
    meter.prepare(sampleRate, ch);
    mono.assign((size_t) juce::jmax(samplesPerBlock, 32), 0.0f);
    outGain.reset(sampleRate, 0.03);
    outGain.setCurrentAndTargetValue(juce::Decibels::decibelsToGain(pOutput->load()));
    seenMatchSerial = -1; seenAmount = -1.0f; seenListen = -1.0f; seenHearAt = -1;
    worker->prepare(sampleRate);
}

void EpsilonPhonProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals nd;
    const auto t0 = juce::Time::getHighResolutionTicks();
    const int num = buffer.getNumSamples(), nch = juce::jmin(2, buffer.getNumChannels());
    for (int c = getTotalNumInputChannels(); c < buffer.getNumChannels(); ++c) buffer.clear(c, 0, num);
    if (num == 0 || nch == 0) return;
    const double rate = sr.load();

    if (resetRequest.exchange(false)) meter.resetIntegrated();

    // Match EQ: re-solve only when the curve or amount changes (no allocation).
    const float amount = pMatch->load() * 0.01f;
    const int ms = matchSerial.load();
    if (ms != seenMatchSerial || std::abs(amount - seenAmount) > 1e-4f)
    {
        seenMatchSerial = ms; seenAmount = amount;
        std::array<double, phon::OctaveEq::numBandsEq> want {};
        for (int i = 0; i < phon::OctaveEq::numBandsEq; ++i) want[(size_t) i] = matchWant[(size_t) i].load() * amount;
        solvedMatch = phon::OctaveEq::solve(want, rate);
        matchEq.setGains(solvedMatch);
    }
    const float listen = pListen->load();
    const int hearAt = (int) pHearAt->load();
    if (hearAt != seenHearAt || std::abs(listen - seenListen) > 1e-3f)
    {
        seenHearAt = hearAt; seenListen = listen;
        std::array<double, phon::OctaveEq::numBandsEq> g {};
        if (hearAt > 0) g = phon::OctaveEq::solve(phon::earSimCurve(listen, hearAtLevel(hearAt)), rate);
        simEq.setGains(g);
    }

    // Export path
    for (int off = 0; off < num; off += 512)
    {
        const int k = juce::jmin(512, num - off);
        float* chs[2] = { buffer.getWritePointer(0, off), buffer.getWritePointer(nch > 1 ? 1 : 0, off) };
        matchEq.process(chs, nch, k);
    }
    outGain.setTargetValue(juce::Decibels::decibelsToGain(pOutput->load()));
    if (outGain.isSmoothing()) { for (int i = 0; i < num; ++i) { const float g = outGain.getNextValue(); for (int c = 0; c < nch; ++c) buffer.getWritePointer(c)[i] *= g; } }
    else if (std::abs(outGain.getTargetValue() - 1.0f) > 1e-6f) for (int c = 0; c < nch; ++c) buffer.applyGain(c, 0, num, outGain.getTargetValue());

    // Taps: loudness + analyser see exactly what will be bounced.
    {
        const float* chs[2] = { buffer.getReadPointer(0), buffer.getReadPointer(nch > 1 ? 1 : 0) };
        meter.process(chs, nch, num);
        for (int off = 0; off < num; off += (int) mono.size())
        {
            const int k = juce::jmin((int) mono.size(), num - off);
            for (int i = 0; i < k; ++i) mono[(size_t) i] = nch > 1 ? 0.5f * (chs[0][off + i] + chs[1][off + i]) : chs[0][off + i];
            ring.push(mono.data(), k);
        }
        loudCountdown -= num;
        if (loudCountdown <= 0)
        {
            loudCountdown = (int) (rate * 0.1);
            const auto ls = meter.state();
            const juce::SpinLock::ScopedTryLockType tl(loudLock);
            if (tl.isLocked()) loudShared = ls;
        }
    }

    // Monitor-only: Hear at
    for (int off = 0; off < num; off += 512)
    {
        const int k = juce::jmin(512, num - off);
        float* chs[2] = { buffer.getWritePointer(0, off), buffer.getWritePointer(nch > 1 ? 1 : 0, off) };
        simEq.process(chs, nch, k);
    }

    const double secs = juce::Time::highResolutionTicksToSeconds(juce::Time::getHighResolutionTicks() - t0);
    const float pct = (float) (secs / (num / rate) * 100.0);
    cpu.store(cpu.load() * 0.95f + pct * 0.05f);
}

//==============================================================================
EpsilonPhonProcessor::Snapshot EpsilonPhonProcessor::getSnapshot() const { const juce::ScopedLock sl(snapLock); return snap; }

void EpsilonPhonProcessor::resetAnalysis()
{
    resetRequest = true;
    worker->requestReset();
}

phon::Profile EpsilonPhonProcessor::currentTarget() const
{
    const int t = (int) pTarget->load();
    if (t == 0) return getReference();
    return phon::genreProfile((phon::Genre) juce::jlimit(0, (int) phon::Genre::count - 1, t - 1));
}

float EpsilonPhonProcessor::loudnessTarget() const
{
    switch ((int) pLoudTarget->load())
    {
        case 1: return -14.0f;
        case 2: return -16.0f;
        case 3: return -7.0f;
        default: { const auto p = currentTarget(); return p.valid ? p.lufs : -9.0f; }
    }
}

void EpsilonPhonProcessor::learnMatch()
{
    const auto s = getSnapshot();
    const auto c = phon::compare(s.average, currentTarget(), pListen->load());
    if (! c.valid) return;
    const auto m = phon::matchCurve(c);
    for (int i = 0; i < phon::OctaveEq::numBandsEq; ++i) matchWant[(size_t) i].store((float) m[(size_t) i]);
    matchLearned = true;
    ++matchSerial;
    if (pMatch->load() < 1.0f)
        if (auto* p = state.getParameter(idMatch)) { p->beginChangeGesture(); p->setValueNotifyingHost(p->convertTo0to1(100.0f)); p->endChangeGesture(); }
}

void EpsilonPhonProcessor::clearMatch()
{
    for (auto& w : matchWant) w.store(0.0f);
    matchLearned = false;
    ++matchSerial;
}

std::array<double, phon::OctaveEq::numBandsEq> EpsilonPhonProcessor::getMatchCurve() const
{
    std::array<double, phon::OctaveEq::numBandsEq> m {};
    for (int i = 0; i < phon::OctaveEq::numBandsEq; ++i) m[(size_t) i] = matchWant[(size_t) i].load();
    return m;
}

double EpsilonPhonProcessor::matchResponseDb(double hz) const
{
    // Drawn on the message thread from the requested curve (not the audio thread's live state).
    const double rate = sr.load();
    std::array<double, phon::OctaveEq::numBandsEq> want {};
    const float amount = pMatch->load() * 0.01f;
    for (int i = 0; i < phon::OctaveEq::numBandsEq; ++i) want[(size_t) i] = matchWant[(size_t) i].load() * amount;
    static thread_local std::array<double, phon::OctaveEq::numBandsEq> lastWant {}, lastSolved {};
    static thread_local double lastRate = 0;
    if (want != lastWant || rate != lastRate) { lastWant = want; lastRate = rate; lastSolved = phon::OctaveEq::solve(want, rate); }
    double r = 0;
    for (int i = 0; i < phon::OctaveEq::numBandsEq; ++i)
    {
        phon::Biquad b; b.setPeak(rate, phon::OctaveEq::safeF(i, rate), phon::OctaveEq::q, lastSolved[(size_t) i]);
        r += b.magDb(rate, hz);
    }
    return r;
}

//==============================================================================
// Reference records: decode on a background thread, average their tonal balance + loudness.
namespace
{
struct RefResult { phon::Bands db {}; float lufs = -70, tp = -150; bool ok = false; };

RefResult analyseFile(const juce::File& f, juce::AudioFormatManager& fm, juce::Thread& th)
{
    RefResult r;
    std::unique_ptr<juce::AudioFormatReader> rd(fm.createReaderFor(f));
    if (rd == nullptr || rd->sampleRate <= 0) return r;
    const int ch = (int) juce::jmin<juce::uint32>(2, rd->numChannels);
    const auto len = juce::jmin<juce::int64>(rd->lengthInSamples, (juce::int64) (rd->sampleRate * 60 * 12));
    phon::Spectrum sp; sp.prepare(rd->sampleRate);
    phon::LoudnessMeter lm; lm.prepare(rd->sampleRate, ch);
    juce::AudioBuffer<float> buf(ch, 65536);
    std::vector<float> mono(65536);
    for (juce::int64 pos = 0; pos < len && ! th.threadShouldExit(); pos += 65536)
    {
        const int k = (int) juce::jmin<juce::int64>(65536, len - pos);
        rd->read(&buf, 0, k, pos, true, ch > 1);
        const float* chs[2] = { buf.getReadPointer(0), buf.getReadPointer(ch > 1 ? 1 : 0) };
        lm.process(chs, ch, k);
        for (int i = 0; i < k; ++i) mono[(size_t) i] = ch > 1 ? 0.5f * (chs[0][i] + chs[1][i]) : chs[0][i];
        sp.push(mono.data(), k);
    }
    const auto ls = lm.state();
    if (! ls.hasIntegrated || sp.frameCount() < 4) return r;
    r.db = sp.averageDb(); r.lufs = ls.integrated; r.tp = ls.truePeak; r.ok = true;
    return r;
}
}

void EpsilonPhonProcessor::loadReferences(const juce::Array<juce::File>& files)
{
    if (files.isEmpty() || loadingRefs.load()) return;
    if (refThread) refThread->stopThread(4000);
    loadingRefs = true;
    { const juce::ScopedLock sl(refLock); refMessage = "Analysing " + juce::String(files.size()) + (files.size() == 1 ? " record..." : " records..."); }
    ++refSerial;

    struct T : juce::Thread
    {
        T(EpsilonPhonProcessor& o, juce::Array<juce::File> f) : juce::Thread("Epsilon Phon reference"), owner(o), files(std::move(f)) {}
        void run() override
        {
            juce::AudioFormatManager fm; fm.registerBasicFormats();
            std::vector<RefResult> res;
            juce::StringArray names;
            for (auto& f : files)
            {
                if (threadShouldExit()) break;
                auto r = analyseFile(f, fm, *this);
                if (r.ok) { res.push_back(r); names.add(f.getFileNameWithoutExtension()); }
            }
            phon::Profile p;
            if (! res.empty())
            {
                // Align each record's 63 Hz-8 kHz median to 0 dB (loudness-match), then average in dB.
                for (auto& r : res)
                {
                    std::vector<float> v;
                    for (int b = 0; b < phon::numBands; ++b) if (phon::bandCentre(b) >= 63 && phon::bandCentre(b) <= 8100) v.push_back(r.db[(size_t) b]);
                    std::sort(v.begin(), v.end());
                    const float med = v[v.size() / 2];
                    for (auto& x : r.db) x -= med;
                }
                double lu = 0, plr = 0;
                for (int b = 0; b < phon::numBands; ++b)
                {
                    double s = 0; for (auto& r : res) s += r.db[(size_t) b];
                    p.db[(size_t) b] = (float) (s / (double) res.size());
                }
                for (auto& r : res) { lu += r.lufs; plr += r.tp - r.lufs; }
                p.lufs = (float) (lu / (double) res.size());
                p.plr = (float) (plr / (double) res.size());
                p.count = (int) res.size();
                p.name = res.size() == 1 ? names[0].toStdString() : (juce::String((int) res.size()) + " hits averaged").toStdString();
                p.valid = true;
            }
            {
                const juce::ScopedLock sl(owner.refLock);
                if (p.valid)
                {
                    owner.reference = p;
                    owner.refMessage = juce::String(p.name) + juce::String::fromUTF8("  \xc2\xb7  ") + juce::String(p.lufs, 1) + " LUFS  "
                                     + juce::String::fromUTF8("\xc2\xb7  PLR ") + juce::String(p.plr, 1);
                }
                else owner.refMessage = "Couldn't read those files (WAV, AIFF, FLAC, MP3 on macOS)";
            }
            owner.loadingRefs = false;
            ++owner.refSerial;                       // the editor polls this serial
        }
        EpsilonPhonProcessor& owner;
        juce::Array<juce::File> files;
    };
    refThread = std::make_unique<T>(*this, files);
    refThread->startThread(juce::Thread::Priority::background);
}

//==============================================================================
void EpsilonPhonProcessor::getStateInformation(juce::MemoryBlock& dest)
{
    auto s = state.copyState();
    s.removeChild(s.getChildWithName("Reference"), nullptr);
    s.removeChild(s.getChildWithName("Match"), nullptr);
    const auto ref = getReference();
    if (ref.valid)
    {
        juce::ValueTree r("Reference");
        r.setProperty("name", juce::String(ref.name), nullptr);
        r.setProperty("lufs", ref.lufs, nullptr);
        r.setProperty("plr", ref.plr, nullptr);
        r.setProperty("count", ref.count, nullptr);
        juce::StringArray v; for (auto x : ref.db) v.add(juce::String(x, 2));
        r.setProperty("bands", v.joinIntoString(" "), nullptr);
        s.appendChild(r, nullptr);
    }
    juce::ValueTree m("Match");
    m.setProperty("learned", matchLearned.load(), nullptr);
    juce::StringArray mv; for (auto& x : matchWant) mv.add(juce::String(x.load(), 3));
    m.setProperty("curve", mv.joinIntoString(" "), nullptr);
    s.appendChild(m, nullptr);
    if (auto xml = s.createXml()) copyXmlToBinary(*xml, dest);
}

void EpsilonPhonProcessor::setStateInformation(const void* data, int size)
{
    auto xml = getXmlFromBinary(data, size);
    if (xml == nullptr || ! xml->hasTagName(state.state.getType())) return;
    auto s = juce::ValueTree::fromXml(*xml);
    const auto r = s.getChildWithName("Reference");
    if (r.isValid())
    {
        phon::Profile p;
        p.name = r["name"].toString().toStdString();
        p.lufs = (float) r["lufs"]; p.plr = (float) r["plr"]; p.count = (int) r["count"];
        juce::StringArray v; v.addTokens(r["bands"].toString(), " ", {});
        if (v.size() == phon::numBands)
        {
            for (int b = 0; b < phon::numBands; ++b) p.db[(size_t) b] = v[b].getFloatValue();
            p.valid = true;
            const juce::ScopedLock sl(refLock);
            reference = p;
            refMessage = juce::String(p.name) + juce::String::fromUTF8("  \xc2\xb7  ") + juce::String(p.lufs, 1) + " LUFS";
        }
    }
    const auto m = s.getChildWithName("Match");
    if (m.isValid())
    {
        juce::StringArray v; v.addTokens(m["curve"].toString(), " ", {});
        if (v.size() == phon::OctaveEq::numBandsEq)
            for (int i = 0; i < v.size(); ++i) matchWant[(size_t) i].store(juce::jlimit(-12.0f, 12.0f, v[i].getFloatValue()));
        matchLearned = (bool) m["learned"];
        ++matchSerial;
    }
    s.removeChild(r, nullptr);
    s.removeChild(m, nullptr);
    state.replaceState(s);
    ++refSerial;
}

juce::AudioProcessorEditor* EpsilonPhonProcessor::createEditor() { return new EpsilonPhonEditor(*this); }
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new EpsilonPhonProcessor(); }
