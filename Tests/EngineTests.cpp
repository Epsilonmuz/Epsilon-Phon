// Epsilon Phon engine tests (no JUCE). Build: see CMakeLists.txt target PhonEngineTests.
#include "PhonEngine.h"
#include <chrono>
#include <cstdio>
#include <random>

using namespace phon;
static int fails = 0;
static void check(bool ok, const char* what, double v = 0)
{
    std::printf("[%s] %s  (%.3f)\n", ok ? "PASS" : "FAIL", what, v);
    if (! ok) ++fails;
}

static std::vector<float> pink(int n, unsigned seed)
{
    // Paul Kellet's refined pink filter
    std::mt19937 rng(seed); std::normal_distribution<float> nd(0.0f, 1.0f);
    std::vector<float> o((size_t) n);
    double b0 = 0, b1 = 0, b2 = 0, b3 = 0, b4 = 0, b5 = 0, b6 = 0;
    for (int i = 0; i < n; ++i)
    {
        const double w = nd(rng);
        b0 = 0.99886 * b0 + w * 0.0555179; b1 = 0.99332 * b1 + w * 0.0750759; b2 = 0.96900 * b2 + w * 0.1538520;
        b3 = 0.86650 * b3 + w * 0.3104856; b4 = 0.55000 * b4 + w * 0.5329522; b5 = -0.7616 * b5 - w * 0.0168980;
        o[(size_t) i] = (float) ((b0 + b1 + b2 + b3 + b4 + b5 + b6 + w * 0.5362) * 0.05);
        b6 = w * 0.115926;
    }
    return o;
}

int main()
{
    // ---- ISO 226
    check(std::abs(iso226::splForPhon(1000, 40) - 40) < 0.05, "ISO226: 40 phon at 1 kHz = 40 dB SPL", iso226::splForPhon(1000, 40));
    check(std::abs(iso226::splForPhon(1000, 80) - 80) < 0.05, "ISO226: 80 phon at 1 kHz = 80 dB SPL", iso226::splForPhon(1000, 80));
    const double s100 = iso226::splForPhon(100, 40);
    check(s100 > 60 && s100 < 65, "ISO226: 40 phon at 100 Hz ~ 62 dB SPL", s100);
    const double s20 = iso226::splForPhon(20, 40);
    check(s20 > 97 && s20 < 102, "ISO226: 40 phon at 20 Hz ~ 99.9 dB SPL", s20);
    const double s3k = iso226::splForPhon(3150, 60);
    check(s3k < 60, "ISO226: ear most sensitive near 3-4 kHz (60 phon needs < 60 dB)", s3k);
    double worst = 0;
    for (double f : { 31.5, 63.0, 125.0, 500.0, 2000.0, 4000.0, 8000.0, 12500.0 })
        for (double L : { 30.0, 50.0, 70.0, 90.0 })
            worst = std::max(worst, std::abs(iso226::phonForSpl(f, iso226::splForPhon(f, L)) - L));
    check(worst < 0.1, "ISO226: SPL->phon inverts phon->SPL (20-90 phon)", worst);
    // Fletcher-Munson in one line: bass falls away faster as you turn down
    const double bassQuiet = iso226::earResponse(50, 40), bassLoud = iso226::earResponse(50, 90);
    check(bassQuiet < bassLoud - 15, "ISO226: 50 Hz is >15 dB weaker at 40 phon than at 90 phon", bassLoud - bassQuiet);

    // ---- ear-sim curve
    const auto sim = earSimCurve(80, 50);
    check(sim[0] < -8 && std::abs(sim[5]) < 0.6, "Hear-at 50 from 80 phon: 31 Hz cut, 1 kHz untouched", sim[0]);
    const auto simLoud = earSimCurve(80, 90);
    check(simLoud[0] > 2, "Hear-at 90 from 80 phon: bass comes up", simLoud[0]);

    for (double sr : { 44100.0, 48000.0 })
    {
        // ---- spectrum: pink noise is flat in third octaves
        Spectrum sp; sp.prepare(sr);
        const auto pn = pink((int) (sr * 20), 7);
        for (size_t i = 0; i < pn.size(); i += 512) sp.push(pn.data() + i, (int) std::min<size_t>(512, pn.size() - i));
        const auto avg = sp.averageDb();
        float mn = 999, mx = -999;
        for (int b = 0; b < numBands; ++b) if (bandCentre(b) >= 50 && bandCentre(b) <= 10000) { mn = std::min(mn, avg[(size_t) b]); mx = std::max(mx, avg[(size_t) b]); }
        char msg[120]; std::snprintf(msg, sizeof msg, "Spectrum @%.0f: pink noise flat 50 Hz-10 kHz within 3 dB", sr);
        check(mx - mn < 3.0f, msg, mx - mn);

        // sine level
        Spectrum s2; s2.prepare(sr);
        std::vector<float> sine((size_t) (sr * 3));
        for (size_t i = 0; i < sine.size(); ++i) sine[i] = 0.1f * (float) std::sin(2 * pi * 1000.0 * i / sr);
        s2.push(sine.data(), (int) sine.size());
        const float l1k = s2.averageDb()[16];
        std::snprintf(msg, sizeof msg, "Spectrum @%.0f: 1 kHz sine amp 0.1 reads -23 dB in the 1 kHz band", sr);
        check(std::abs(l1k + 23.0f) < 0.8f, msg, l1k);

        // ---- loudness: EBU Tech 3341 case 1 (stereo 1 kHz sine at -23 dBFS reads -23 LUFS)
        LoudnessMeter lm; lm.prepare(sr, 2);
        const int len = (int) (sr * 20);
        std::vector<float> L((size_t) len), R((size_t) len);
        const float a = std::pow(10.0f, -23.0f / 20.0f);
        for (int i = 0; i < len; ++i) L[(size_t) i] = R[(size_t) i] = a * (float) std::sin(2 * pi * 1000.0 * i / sr);
        const float* chs[2] = { L.data(), R.data() };
        for (int i = 0; i < len; i += 480) { const float* c[2] = { chs[0] + i, chs[1] + i }; lm.process(c, 2, std::min(480, len - i)); }
        auto st = lm.state();
        std::snprintf(msg, sizeof msg, "Loudness @%.0f: -23 dBFS stereo sine = -23.0 LUFS integrated", sr);
        check(st.hasIntegrated && std::abs(st.integrated + 23.0f) < 0.2f, msg, st.integrated);
        std::snprintf(msg, sizeof msg, "Loudness @%.0f: short-term -23 LUFS", sr);
        check(std::abs(st.shortTerm + 23.0f) < 0.2f, msg, st.shortTerm);
        // silence is gated out
        std::vector<float> z((size_t) len, 0.0f);
        for (int i = 0; i < len; i += 480) { const float* c[2] = { z.data() + i, z.data() + i }; lm.process(c, 2, std::min(480, len - i)); }
        st = lm.state();
        std::snprintf(msg, sizeof msg, "Loudness @%.0f: 20 s of silence after does not drag integrated down", sr);
        check(std::abs(st.integrated + 23.0f) < 0.2f, msg, st.integrated);

        // ---- true peak: fs/4 sine at 45 deg has samples at 0.707 but a true peak of 1.0
        LoudnessMeter tp; tp.prepare(sr, 1);
        std::vector<float> q(48000);
        for (size_t i = 0; i < q.size(); ++i) q[i] = (float) std::sin(2 * pi * i / 4.0 + pi / 4);
        const float* qc[1] = { q.data() };
        tp.process(qc, 1, (int) q.size());
        std::snprintf(msg, sizeof msg, "True peak @%.0f: inter-sample peak found (samples -3 dB, true 0 dBTP)", sr);
        check(std::abs(tp.state().truePeak) < 0.6f, msg, tp.state().truePeak);

        // ---- octave EQ: solved curve lands on target
        std::array<double, OctaveEq::numBandsEq> want {}; want[3] = 3.0;    // +3 dB at 250 Hz
        const auto g = OctaveEq::solve(want, sr);
        OctaveEq eq; eq.prepare(sr, 2);
        eq.setGains(g);
        std::vector<float> s250((size_t) sr), r250((size_t) sr);
        for (size_t i = 0; i < s250.size(); ++i) s250[i] = r250[i] = 0.25f * (float) std::sin(2 * pi * 250.0 * i / sr);
        float* io[2] = { s250.data(), r250.data() };
        for (int blk = 0; blk < 40; ++blk) { float* c[2] = { io[0], io[1] }; eq.process(c, 2, 1); }   // ramp in
        for (size_t i = 0; i < s250.size(); ++i) s250[i] = r250[i] = 0.25f * (float) std::sin(2 * pi * 250.0 * i / sr);
        for (int i = 0; i < (int) sr; i += 512) { float* c[2] = { io[0] + i, io[1] + i }; eq.process(c, 2, std::min(512, (int) sr - i)); }
        double pk = 0; for (size_t i = s250.size() / 2; i < s250.size(); ++i) pk = std::max(pk, (double) std::abs(s250[i]));
        const double gdb = 20 * std::log10(pk / 0.25);
        std::snprintf(msg, sizeof msg, "OctaveEq @%.0f: +3 dB at 250 Hz measured", sr);
        check(std::abs(gdb - 3.0) < 0.35, msg, gdb);
        std::snprintf(msg, sizeof msg, "OctaveEq @%.0f: 1 kHz stays within 0.5 dB", sr);
        check(std::abs(eq.responseDb(1000)) < 0.5, msg, eq.responseDb(1000));
    }

    // ---- compare
    const auto trap = genreProfile(Genre::Trap);
    Bands mix = trap.db; for (auto& v : mix) v -= 31.0f;    // same shape, different level
    auto c = compare(mix, trap, 79.0f);
    check(c.valid && c.score > 99.0f, "Compare: same curve at another level scores ~100", c.score);
    for (int b = 0; b < numBands; ++b) if (bandInRange(b, 2)) mix[(size_t) b] += 3.0f;   // +3 dB low-mid
    c = compare(mix, trap, 79.0f);
    check(c.rangeDb[2] > 2.0f && c.rangeDb[2] < 3.1f, "Compare: +3 dB low-mid shows in the Low-mid range", c.rangeDb[2]);
    check(c.score < 99.0f, "Compare: score drops", c.score);
    LoudnessState ls; ls.hasIntegrated = true; ls.integrated = -12.0f; ls.truePeak = -1.5f;
    auto tips = suggestions(c, trap, ls, -8.0f);
    bool hasLowMid = false, hasLufs = false;
    for (auto& t : tips) { std::printf("      tip: %s\n", t.c_str()); hasLowMid |= t.find("Low-mid") != std::string::npos; hasLufs |= t.find("LUFS") != std::string::npos; }
    check(hasLowMid && hasLufs, "Suggestions: names the heavy range and the loudness gap");

    // sub deficit hurts more at low volume (equal-loudness growth is steeper in the bass)
    Bands thin = trap.db;
    for (int b = 0; b < numBands; ++b) if (bandInRange(b, 0)) thin[(size_t) b] -= 4.0f;
    c = compare(thin, trap, 79.0f);
    check(c.rangePhon[0][0] < c.rangePhon[2][0] - 0.5f, "Translation: -4 dB sub sounds thinner at 60 than at 90 dB SPL (sones)", c.rangePhon[0][0] - c.rangePhon[2][0]);
    const auto mc = matchCurve(c);
    check(mc[0] > 2.0, "Match: proposes a sub boost for a thin sub", mc[0]);

    // ---- real time: the quick analyser agrees with the long one and flags what pokes out
    {
        const double sr = 48000;
        Spectrum big, quick; big.prepare(sr, 14); quick.prepare(sr, 12);
        auto pn = pink((int) sr * 6, 21);
        big.push(pn.data(), (int) pn.size()); quick.push(pn.data(), (int) pn.size());
        const auto A = big.averageDb(), Q = quick.averageDb();
        float worstQ = 0; for (int b = 10; b < 27; ++b) worstQ = std::max(worstQ, std::abs(A[(size_t) b] - Q[(size_t) b]));
        check(worstQ < 1.0f, "Live: fast (4096) analyser matches the long one 250 Hz-12.5 kHz", worstQ);

        // flat-in-third-octaves target; pink is on target, then a 2 kHz tone appears
        Profile flat; flat.valid = true; flat.db.fill(0.0f);
        auto ld = liveDeviation(quick.quickDb(), flat);
        float mx = 0; for (int b = 10; b < 27; ++b) mx = std::max(mx, std::abs(ld.dev[(size_t) b]));
        check(ld.valid && mx < 4.0f, "Live: pink noise on a pink target shows nothing sticking out", mx);
        std::vector<float> tone((size_t) (sr * 0.25));
        for (size_t i = 0; i < tone.size(); ++i) tone[i] = pn[i] + 0.12f * (float) std::sin(2 * pi * 2000.0 * i / sr);
        quick.push(tone.data(), (int) tone.size());
        ld = liveDeviation(quick.quickDb(), flat);
        Offender off[3]; const int n = offenders(ld.dev, 3.0f, true, off, 3);
        check(n >= 1 && off[0].band == 19, "Live: a 2 kHz tone is named as too loud within 250 ms", n ? off[0].db : 0);
        Bands silent; silent.fill(-150.0f);
        check(! liveDeviation(silent, flat).valid, "Live: silence is gated (no false alarms)");
        LiveTracker tr; tr.update(ld, 0.05f);
        const float h0 = tr.hold[19];
        LiveDev none; none.valid = true;
        for (int i = 0; i < 10; ++i) tr.update(none, 0.05f);
        check(tr.hold[19] == h0, "Live: peak hold stays 0.5 s after the peak", tr.hold[19]);
        for (int i = 0; i < 60; ++i) tr.update(none, 0.05f);
        check(tr.hold[19] < 0.5f && std::abs(tr.smooth[19]) < 0.5f, "Live: hold and smoothed value release", tr.hold[19]);
    }

    // every genre profile is finite and loud enough to compare
    for (int gi = 0; gi < (int) Genre::count; ++gi)
    {
        auto p = genreProfile((Genre) gi);
        bool ok = true; for (auto v : p.db) ok &= std::isfinite(v);
        char msg[80]; std::snprintf(msg, sizeof msg, "Genre profile finite: %s", genreName((Genre) gi));
        check(ok && compare(p.db, p, 79).valid, msg);
    }

    // ---- speed: 60 s stereo at 48 kHz through both EQs + meter + spectrum
    {
        const double sr = 48000; const int n = (int) sr * 60;
        auto pn = pink(n, 3);
        std::vector<float> l = pn, r = pn;
        OctaveEq a, b; a.prepare(sr, 2); b.prepare(sr, 2);
        a.setGains(OctaveEq::solve(matchCurve(c), sr)); b.setGains(OctaveEq::solve(earSimCurve(80, 55), sr));
        LoudnessMeter m; m.prepare(sr, 2); Spectrum s; s.prepare(sr);
        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < n; i += 512)
        {
            const int k = std::min(512, n - i);
            float* ch[2] = { l.data() + i, r.data() + i };
            a.process(ch, 2, k);
            const float* cc[2] = { ch[0], ch[1] };
            m.process(cc, 2, k);
            b.process(ch, 2, k);
        }
        const double audio = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        const auto t1 = std::chrono::steady_clock::now();
        s.push(l.data(), n);
        const double fft = std::chrono::duration<double>(std::chrono::steady_clock::now() - t1).count();
        check(audio < 3.0, "Speed: audio path uses < 5 % of real time (60 s of audio)", audio / 60.0 * 100.0);
        check(fft < 6.0, "Speed: analysis worker < 10 % of real time", fft / 60.0 * 100.0);
    }

    std::printf("\n%s (%d failure%s)\n", fails ? "FAILED" : "ALL PASSED", fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
