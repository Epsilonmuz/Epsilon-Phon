#pragma once
// Epsilon Phon engine - header-only, no JUCE, no allocation after prepare().
//
//  iso226     ISO 226 equal-loudness-level contours (2003 parameters; the 2023 edition differs by
//             at most 0.6 dB, Suzuki et al. 2024) both ways: phon -> SPL and SPL -> phon.
//  Bands      29 third-octave bands, 25 Hz .. 16 kHz.
//  Spectrum   FFT long-term-average spectrum per band (fast display + integrated average).
//  Targets    Modelled genre tonal-balance curves, or a profile learned from real hit records.
//  Compare    Loudness-matched deviation in dB and in phon at any listening level, per range,
//             a translation score and plain-language suggestions.
//  OctaveEq   10-band zero-latency octave EQ with a solver so the curve lands on the target gains.
//  Loudness   ITU-R BS.1770-4 / EBU R128: momentary, short-term, gated integrated, 4x true peak.
#include <array>
#include <atomic>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>
#include <algorithm>

namespace phon
{
constexpr double pi = 3.14159265358979323846;

//==============================================================================
namespace iso226
{
    // ISO 226:2003 Table 1 (free field, frontal incidence, pure tones).
    constexpr int numF = 29;
    constexpr double f[numF]  = { 20, 25, 31.5, 40, 50, 63, 80, 100, 125, 160, 200, 250, 315, 400, 500, 630, 800,
                                  1000, 1250, 1600, 2000, 2500, 3150, 4000, 5000, 6300, 8000, 10000, 12500 };
    constexpr double af[numF] = { 0.532, 0.506, 0.480, 0.455, 0.432, 0.409, 0.387, 0.367, 0.349, 0.330, 0.315, 0.301,
                                  0.288, 0.276, 0.267, 0.259, 0.253, 0.250, 0.246, 0.244, 0.243, 0.243, 0.243, 0.242,
                                  0.242, 0.245, 0.254, 0.271, 0.301 };
    constexpr double Lu[numF] = { -31.6, -27.2, -23.0, -19.1, -15.9, -13.0, -10.3, -8.1, -6.2, -4.5, -3.1, -2.0, -1.1,
                                  -0.4, 0.0, 0.3, 0.5, 0.0, -2.7, -4.1, -1.0, 1.7, 2.5, 1.2, -2.1, -7.1, -11.2, -10.7, -3.1 };
    constexpr double Tf[numF] = { 78.5, 68.7, 59.5, 51.1, 44.0, 37.5, 31.5, 26.5, 22.1, 17.9, 14.4, 11.4, 8.6, 6.2,
                                  4.4, 3.0, 2.2, 2.4, 3.5, 1.7, -1.3, -4.2, -6.0, -5.4, -1.5, 6.0, 12.6, 13.9, 12.3 };

    struct Params { double af, Lu, Tf; };

    // Parameters at any frequency: linear in log-frequency between table points, clamped at the ends.
    inline Params at(double hz)
    {
        if (hz <= f[0]) return { af[0], Lu[0], Tf[0] };
        if (hz >= f[numF - 1]) return { af[numF - 1], Lu[numF - 1], Tf[numF - 1] };
        int i = 0;
        while (i < numF - 2 && hz > f[i + 1]) ++i;
        const double t = (std::log(hz) - std::log(f[i])) / (std::log(f[i + 1]) - std::log(f[i]));
        return { af[i] + t * (af[i + 1] - af[i]), Lu[i] + t * (Lu[i + 1] - Lu[i]), Tf[i] + t * (Tf[i + 1] - Tf[i]) };
    }

    // ISO 226 clause 4.1: sound pressure level (dB SPL) that sounds as loud as `phonLevel`.
    inline double splForPhon(double hz, double phonLevel)
    {
        const auto p = at(hz);
        const double Af = 4.47e-3 * (std::pow(10.0, 0.025 * phonLevel) - 1.15)
                        + std::pow(0.4 * std::pow(10.0, (p.Tf + p.Lu) / 10.0 - 9.0), p.af);
        return 10.0 / p.af * std::log10(std::max(Af, 1e-30)) - p.Lu + 94.0;
    }

    // ISO 226 clause 4.2: loudness level (phon) of a tone at `spl` dB. Below threshold returns 0.
    inline double phonForSpl(double hz, double spl)
    {
        const auto p = at(hz);
        if (spl <= p.Tf) return 0.0;
        const double Bf = std::pow(0.4 * std::pow(10.0, (spl + p.Lu) / 10.0 - 9.0), p.af)
                        - std::pow(0.4 * std::pow(10.0, (p.Tf + p.Lu) / 10.0 - 9.0), p.af) + 0.005135;
        return std::max(0.0, 40.0 * std::log10(std::max(Bf, 1e-30)) + 94.0);
    }

    // How much quieter (negative) or louder a frequency sounds than 1 kHz at the same SPL,
    // for a listener at `phonLevel`: the ear's own "EQ curve" at that volume.
    inline double earResponse(double hz, double phonLevel) { return -(splForPhon(hz, phonLevel) - phonLevel); }
}

//==============================================================================
constexpr int numBands = 29;
inline double bandCentre(int b) { return 1000.0 * std::pow(10.0, (b - 16) / 10.0); }       // 25.1 Hz .. 15.85 kHz
inline double bandLo(int b)     { return bandCentre(b) * std::pow(10.0, -0.05); }
inline double bandHi(int b)     { return bandCentre(b) * std::pow(10.0, 0.05); }
inline const char* bandLabel(int b)
{
    static const char* l[numBands] = { "25", "31", "40", "50", "63", "80", "100", "125", "160", "200", "250", "315", "400",
                                       "500", "630", "800", "1k", "1k25", "1k6", "2k", "2k5", "3k15", "4k", "5k", "6k3",
                                       "8k", "10k", "12k5", "16k" };
    return l[b < 0 ? 0 : b >= numBands ? numBands - 1 : b];
}

using Bands = std::array<float, numBands>;

// The four tonal-balance ranges used in hit-record studies (iZotope Tonal Balance Control's split),
// with sub broken out because it decides how trap / drill / dembow translate.
struct Range { const char* name; double lo, hi; };
constexpr int numRanges = 5;
inline const Range& range(int i)
{
    static const Range r[numRanges] = { { "Sub", 20, 63 }, { "Low", 63, 250 }, { "Low-mid", 250, 1000 },
                                        { "High-mid", 1000, 5000 }, { "High", 5000, 20000 } };
    return r[i < 0 ? 0 : i >= numRanges ? numRanges - 1 : i];
}
inline bool bandInRange(int b, int r) { const auto c = bandCentre(b); return c >= range(r).lo * 0.95 && c < range(r).hi * 0.95; }

//==============================================================================
// In-place radix-2 FFT.
class FFT
{
public:
    void prepare(int order)
    {
        n = 1 << order;
        tw.resize((size_t) n / 2);
        for (int i = 0; i < n / 2; ++i) tw[(size_t) i] = std::polar(1.0f, (float) (-2.0 * pi * i / n));
        rev.resize((size_t) n);
        for (int i = 0; i < n; ++i)
        {
            int r = 0;
            for (int b = 0; b < order; ++b) if (i & (1 << b)) r |= 1 << (order - 1 - b);
            rev[(size_t) i] = r;
        }
    }
    int size() const { return n; }
    void perform(std::complex<float>* x) const
    {
        for (int i = 0; i < n; ++i) if (i < rev[(size_t) i]) std::swap(x[i], x[rev[(size_t) i]]);
        for (int len = 2; len <= n; len <<= 1)
        {
            const int step = n / len;
            for (int i = 0; i < n; i += len)
                for (int j = 0; j < len / 2; ++j)
                {
                    const auto t = tw[(size_t) (j * step)] * x[i + j + len / 2];
                    x[i + j + len / 2] = x[i + j] - t;
                    x[i + j] += t;
                }
        }
    }
private:
    int n = 0;
    std::vector<std::complex<float>> tw;
    std::vector<int> rev;
};

//==============================================================================
// Single-producer / single-consumer float ring (audio thread -> analysis worker). Lock-free.
class Ring
{
public:
    void prepare(int capacityPow2) { buf.assign((size_t) capacityPow2, 0.0f); mask = capacityPow2 - 1; w.store(0); r.store(0); }
    void push(const float* x, int num)
    {
        auto wi = w.load(std::memory_order_relaxed);
        const auto ri = r.load(std::memory_order_acquire);
        const int space = (int) buf.size() - (int) (wi - ri);
        num = std::min(num, space);                       // drop on overflow, never block the audio thread
        for (int i = 0; i < num; ++i) buf[(size_t) ((wi + (uint64_t) i) & (uint64_t) mask)] = x[i];
        w.store(wi + (uint64_t) num, std::memory_order_release);
    }
    int pop(float* x, int maxNum)
    {
        const auto wi = w.load(std::memory_order_acquire);
        auto ri = r.load(std::memory_order_relaxed);
        const int num = std::min(maxNum, (int) (wi - ri));
        for (int i = 0; i < num; ++i) x[i] = buf[(size_t) ((ri + (uint64_t) i) & (uint64_t) mask)];
        r.store(ri + (uint64_t) num, std::memory_order_release);
        return num;
    }
private:
    std::vector<float> buf;
    int mask = 0;
    std::atomic<uint64_t> w { 0 }, r { 0 };
};

//==============================================================================
// Third-octave long-term-average spectrum. Hann window, 75 % overlap.
// fast: exponential average (~0.4 s) for the live curve; average: everything since reset().
class Spectrum
{
public:
    // order 14 (16384 points, 2.7-2.9 Hz bins) resolves the 25 Hz bands; order 12 reacts 4x faster
    // and is used for the real-time view above 200 Hz.
    void prepare(double sampleRate, int fftOrder = 14)
    {
        sr = sampleRate;
        fft.prepare(fftOrder);
        N = fft.size();
        hop = N / 4;
        window.resize((size_t) N);
        double wsum = 0.0;
        for (int i = 0; i < N; ++i) { window[(size_t) i] = (float) (0.5 - 0.5 * std::cos(2.0 * pi * i / (N - 1))); wsum += window[(size_t) i] * window[(size_t) i]; }
        norm = (float) (1.0 / (wsum * N) * 2.0);
        hist.assign((size_t) N, 0.0f);
        work.resize((size_t) N);
        for (int b = 0; b < numBands; ++b)
        {
            int lo = (int) std::ceil(bandLo(b) * N / sr), hi = (int) std::floor(bandHi(b) * N / sr);
            lo = std::max(lo, 1); hi = std::min(hi, N / 2 - 1);
            if (hi < lo) { lo = hi = std::max(1, std::min(N / 2 - 1, (int) std::lround(bandCentre(b) * N / sr))); }
            binLo[(size_t) b] = lo; binHi[(size_t) b] = hi;
            valid[(size_t) b] = bandLo(b) < sr * 0.5 * 0.98;
        }
        const double framesPerSec = sr / hop;
        fastCoef = (float) std::exp(-1.0 / (0.4 * framesPerSec));
        quickCoef = (float) std::exp(-1.0 / (0.10 * framesPerSec));
        reset();
    }
    void reset()
    {
        std::fill(hist.begin(), hist.end(), 0.0f);
        fill = 0; frames = 0;
        fastP.fill(0.0); quickP.fill(0.0); sumP.fill(0.0);
    }
    void resetAverage() { sumP.fill(0.0); frames = 0; }

    void push(const float* x, int num)
    {
        for (int i = 0; i < num; ++i)
        {
            hist[(size_t) fill++] = x[i];
            if (fill == N) { frame(); std::move(hist.begin() + hop, hist.end(), hist.begin()); fill = N - hop; }
        }
    }

    // dB per band (power sum in band); -150 when empty.
    Bands fastDb() const { Bands o; for (int b = 0; b < numBands; ++b) o[(size_t) b] = toDb(fastP[(size_t) b]); return o; }
    Bands quickDb() const { Bands o; for (int b = 0; b < numBands; ++b) o[(size_t) b] = toDb(quickP[(size_t) b]); return o; }
    Bands averageDb() const
    {
        Bands o;
        for (int b = 0; b < numBands; ++b) o[(size_t) b] = frames > 0 ? toDb(sumP[(size_t) b] / frames) : -150.0f;
        return o;
    }
    long long frameCount() const { return frames; }
    double secondsAveraged() const { return frames * (double) hop / sr; }
    bool bandValid(int b) const { return valid[(size_t) b]; }

private:
    FFT fft;
    double sr = 48000.0;
    int N = 0, hop = 0, fill = 0;
    float norm = 1.0f, fastCoef = 0.9f, quickCoef = 0.5f;
    std::vector<float> window, hist;
    std::vector<std::complex<float>> work;
    std::array<int, numBands> binLo {}, binHi {};
    std::array<bool, numBands> valid {};
    std::array<double, numBands> fastP {}, quickP {}, sumP {};
    long long frames = 0;

    static float toDb(double p) { return p > 1e-15 ? (float) (10.0 * std::log10(p)) : -150.0f; }
    void frame()
    {
        for (int i = 0; i < N; ++i) work[(size_t) i] = { hist[(size_t) i] * window[(size_t) i], 0.0f };
        fft.perform(work.data());
        for (int b = 0; b < numBands; ++b)
        {
            double p = 0.0;
            for (int k = binLo[(size_t) b]; k <= binHi[(size_t) b]; ++k) p += std::norm(work[(size_t) k]);
            p *= norm;
            if (! valid[(size_t) b]) p = 0.0;
            fastP[(size_t) b] = fastCoef * fastP[(size_t) b] + (1.0 - fastCoef) * p;
            quickP[(size_t) b] = quickCoef * quickP[(size_t) b] + (1.0 - quickCoef) * p;
            sumP[(size_t) b] += p;
        }
        ++frames;
    }
};

//==============================================================================
// Target profiles. Built-in genre curves are MODELS shaped from published averages
// (Pestana/Ma/Reiss AES 2013: ~5 dB/oct per FFT bin 100 Hz-4 kHz, steeper above, hip-hop/electronic
// extended lows and brightest tops; Billboard Year-End study 2025: 2020 hits darker, more low/low-mid,
// -8.8 LUFS integrated, PLR 9.3). For true "standard hits", load real reference records.
struct Profile
{
    std::string name;
    Bands db {};                // relative band levels (any offset; compare() loudness-matches)
    float lufs = -9.0f;         // integrated loudness of the target
    float plr = 9.3f;           // peak-to-loudness ratio (true peak - integrated)
    int count = 0;              // number of records averaged (0 = modelled)
    bool valid = false;
};

enum class Genre { Trap, Drill, Dembow, HipHop, Pop, RnB, EDM, Rock, count };
inline const char* genreName(Genre g)
{
    static const char* n[] = { "Trap", "Drill", "Dembow / Reggaeton", "Hip-Hop", "Pop", "R&B", "EDM", "Rock" };
    return n[(int) g];
}

inline Profile genreProfile(Genre g)
{
    struct S { double slope, top, sub, subF, cut, presence, lufs, plr; };
    // slope: band-power dB/oct 100 Hz-4 kHz (third-octave power; pink = 0), top: extra dB/oct above 4 kHz,
    // sub: bump in dB around subF, cut: low cut-off (18 dB/oct below), presence: 3 kHz bump.
    static const S s[] = {
        { -2.2, -1.0, 7.0, 50, 28, 0.5, -8.0, 8.5 },   // Trap: big 808, bright hats
        { -2.2, -1.2, 7.0, 45, 26, 1.0, -8.0, 8.5 },   // Drill: sliding 808s sit lower
        { -2.0, -1.2, 5.0, 60, 32, 1.0, -7.5, 8.0 },   // Dembow / reggaeton: kick + bass forward
        { -2.3, -1.5, 4.0, 60, 35, 0.0, -9.0, 9.5 },   // Hip-hop (boom bap)
        { -2.0, -2.0, 2.5, 60, 35, 1.0, -9.0, 9.3 },   // Pop (2020 Billboard average loudness)
        { -2.5, -2.5, 3.5, 55, 32, 0.0, -10.0, 10.0 }, // R&B: lighter bottom, smoother top
        { -1.8, -1.2, 5.0, 55, 30, 0.5, -7.0, 7.5 },   // EDM
        { -1.8, -2.5, 0.5, 80, 45, 1.5, -9.0, 9.5 },   // Rock
    };
    const auto& p = s[(int) g];
    Profile pr;
    pr.name = genreName(g);
    for (int b = 0; b < numBands; ++b)
    {
        const double f = bandCentre(b);
        const double o = std::log2(f / 1000.0);
        double d = p.slope * o;
        if (f > 4000.0) d += p.top * std::log2(f / 4000.0);
        const double so = std::log2(f / p.subF) / 0.9;
        d += p.sub * std::exp(-0.5 * so * so);
        const double po = std::log2(f / 3000.0) / 0.8;
        d += p.presence * std::exp(-0.5 * po * po);
        if (f < p.cut) d -= 18.0 * std::log2(p.cut / f);
        if (f > 14000.0) d -= 8.0 * std::log2(f / 14000.0);             // codec / mastering air roll-off
        pr.db[(size_t) b] = (float) d;
    }
    pr.lufs = (float) p.lufs; pr.plr = (float) p.plr; pr.valid = true;
    return pr;
}

//==============================================================================
// Mix vs target. All work in loudness-matched terms: the target is shifted so its 63 Hz-8 kHz
// mean equals the mix's, i.e. "both played at the same volume".
struct Comparison
{
    Bands mixDb {}, targetDb {};              // targetDb already loudness-matched to the mix
    Bands devDb {};                           // mix - target, dB
    std::array<Bands, 3> devPhon {};          // perceived mix - target (sone ratio as phon-dB) at {quiet, your level, loud}
    std::array<float, 3> levels { 60.0f, 79.0f, 90.0f };
    std::array<float, numRanges> rangeDb {};
    std::array<std::array<float, numRanges>, 3> rangePhon {};
    float score = 0.0f;                       // 0..100 translation score
    bool valid = false;
};

// Loudness in sones from loudness level (ISO 532 / Stevens: doubles every 10 phon above 40,
// steeper below - which is why a slightly thin sub vanishes at low volume).
inline double sones(double phonLevel)
{
    if (phonLevel <= 0.0) return 0.0;
    return phonLevel >= 40.0 ? std::pow(2.0, (phonLevel - 40.0) / 10.0) : std::pow(phonLevel / 40.0, 2.642);
}
// Perceived difference in "phon-equivalent dB": 10 * log2(loudness ratio), clamped to +-12.
inline float perceivedDb(double nMix, double nTarget)
{
    if (nTarget < 1e-4 && nMix < 1e-4) return 0.0f;
    return (float) std::clamp(10.0 * std::log2(std::max(nMix, 1e-6) / std::max(nTarget, 1e-6)), -12.0, 12.0);
}

inline bool useBand(int b) { const auto c = bandCentre(b); return c >= 30.0 && c <= 13000.0; }

inline Comparison compare(const Bands& mix, const Profile& target, float listenSpl, float quietSpl = 60.0f, float loudSpl = 90.0f)
{
    Comparison c;
    c.levels = { quietSpl, listenSpl, loudSpl };
    if (! target.valid) return c;
    // Loudness-match on the MEDIAN band difference (63 Hz-8 kHz) so one heavy range doesn't drag the rest.
    std::array<float, numBands> d {}; int n = 0;
    for (int b = 0; b < numBands; ++b)
    {
        const auto f = bandCentre(b);
        if (f < 63.0 || f > 8100.0 || mix[(size_t) b] < -140.0f) continue;
        d[(size_t) n++] = mix[(size_t) b] - target.db[(size_t) b];
    }
    if (n < 8) return c;
    std::sort(d.begin(), d.begin() + n);
    const float off = n % 2 ? d[(size_t) (n / 2)] : 0.5f * (d[(size_t) (n / 2 - 1)] + d[(size_t) (n / 2)]);
    // Absolute scale: the loudest band of the matched target sits ~6 dB under the total listening
    // level (typical for full-range music), so each band gets a plausible SPL at the seat.
    double tot = 0;
    for (int b = 0; b < numBands; ++b) tot += std::pow(10.0, (target.db[(size_t) b] + off) / 10.0);
    const float totDb = (float) (10.0 * std::log10(std::max(tot, 1e-20)));
    std::array<std::array<double, numBands>, 3> nm {}, nt {};
    double sqDb = 0; int nDb = 0;
    for (int b = 0; b < numBands; ++b)
    {
        const float t = target.db[(size_t) b] + off;
        const float m = std::max(mix[(size_t) b], -150.0f);
        c.mixDb[(size_t) b] = m;
        c.targetDb[(size_t) b] = t;
        c.devDb[(size_t) b] = std::clamp(m - t, -40.0f, 40.0f);
        if (useBand(b)) { sqDb += c.devDb[(size_t) b] * c.devDb[(size_t) b]; ++nDb; }
        for (int L = 0; L < 3; ++L)
        {
            const double f = bandCentre(b);
            const double splT = c.levels[(size_t) L] + (t - totDb);
            const double splM = c.levels[(size_t) L] + (m - totDb);
            nt[(size_t) L][(size_t) b] = sones(iso226::phonForSpl(f, splT));
            nm[(size_t) L][(size_t) b] = sones(iso226::phonForSpl(f, splM));
            c.devPhon[(size_t) L][(size_t) b] = perceivedDb(nm[(size_t) L][(size_t) b], nt[(size_t) L][(size_t) b]);
        }
    }
    double sqR = 0; int nR = 0;
    for (int r = 0; r < numRanges; ++r)
    {
        double s = 0; int k = 0; std::array<double, 3> sm2 {}, st2 {};
        for (int b = 0; b < numBands; ++b)
            if (bandInRange(b, r) && useBand(b))
            {
                s += c.devDb[(size_t) b]; ++k;
                for (int L = 0; L < 3; ++L) { sm2[(size_t) L] += nm[(size_t) L][(size_t) b]; st2[(size_t) L] += nt[(size_t) L][(size_t) b]; }
            }
        c.rangeDb[(size_t) r] = k ? (float) (s / k) : 0.0f;
        for (int L = 0; L < 3; ++L)
        {
            const float v = perceivedDb(sm2[(size_t) L], st2[(size_t) L]);
            c.rangePhon[(size_t) L][(size_t) r] = v;
            sqR += v * v; ++nR;
        }
    }
    const double rmsR = nR ? std::sqrt(sqR / nR) : 99.0, rmsDb = nDb ? std::sqrt(sqDb / nDb) : 99.0;
    c.score = (float) std::clamp(100.0 - 6.0 * rmsR - 4.0 * rmsDb, 0.0, 100.0);
    c.valid = true;
    return c;
}

//==============================================================================
// Real time: what is louder (or quieter) than it should be RIGHT NOW.
// The live spectrum is loudness-matched to the target on the median band difference, so the
// answer does not depend on how loud the passage is - only on its balance.
struct LiveDev { Bands dev {}; bool valid = false; };

inline LiveDev liveDeviation(const Bands& live, const Profile& target, float gateDb = -70.0f)
{
    LiveDev o;
    if (! target.valid) return o;
    double tot = 0;
    for (int b = 0; b < numBands; ++b) tot += std::pow(10.0, live[(size_t) b] / 10.0);
    if (10.0 * std::log10(std::max(tot, 1e-30)) < gateDb) return o;          // silence: nothing to judge
    std::array<float, numBands> d {}; int n = 0;
    for (int b = 0; b < numBands; ++b)
    {
        const auto f = bandCentre(b);
        if (f < 63.0 || f > 8100.0) continue;
        d[(size_t) n++] = live[(size_t) b] - target.db[(size_t) b];
    }
    std::sort(d.begin(), d.begin() + n);
    const float off = n % 2 ? d[(size_t) (n / 2)] : 0.5f * (d[(size_t) (n / 2 - 1)] + d[(size_t) (n / 2)]);
    for (int b = 0; b < numBands; ++b)
        o.dev[(size_t) b] = useBand(b) ? std::clamp(live[(size_t) b] - target.db[(size_t) b] - off, -18.0f, 18.0f) : 0.0f;
    o.valid = true;
    return o;
}

// Display ballistics for the live view: instant bars, a readable ~0.5 s smoothed value for the
// call-outs and a 1.5 s peak hold on anything that pokes out.
struct LiveTracker
{
    Bands inst {}, smooth {}, hold {};
    std::array<float, numBands> age {};
    bool active = false;
    void update(const LiveDev& d, float dt)
    {
        active = d.valid;
        const float a = 1.0f - std::exp(-dt / 0.5f);
        for (int b = 0; b < numBands; ++b)
        {
            const float v = d.valid ? d.dev[(size_t) b] : 0.0f;
            inst[(size_t) b] = v;
            smooth[(size_t) b] += a * (v - smooth[(size_t) b]);
            if (v >= hold[(size_t) b]) { hold[(size_t) b] = v; age[(size_t) b] = 0.0f; }
            else if ((age[(size_t) b] += dt) > 1.5f) hold[(size_t) b] = std::max(v, hold[(size_t) b] - 14.0f * dt);
        }
    }
};

// The worst offenders: local maxima of |deviation| beyond `threshold`, strongest first.
// over = true -> too loud; false -> missing. Returns the count written (no allocation).
struct Offender { int band = 0; float db = 0.0f; };
inline int offenders(const Bands& dev, float threshold, bool over, Offender* out, int maxOut)
{
    int n = 0;
    std::array<Offender, numBands> all {};
    for (int b = 0; b < numBands; ++b)
    {
        if (! useBand(b)) continue;
        const float v = over ? dev[(size_t) b] : -dev[(size_t) b];
        if (v < threshold) continue;
        const float l = b > 0 ? (over ? dev[(size_t) b - 1] : -dev[(size_t) b - 1]) : -99.0f;
        const float r = b < numBands - 1 ? (over ? dev[(size_t) b + 1] : -dev[(size_t) b + 1]) : -99.0f;
        if (v >= l && v > r) all[(size_t) n++] = { b, dev[(size_t) b] };
    }
    std::sort(all.begin(), all.begin() + n, [](const Offender& a, const Offender& b) { return std::abs(a.db) > std::abs(b.db); });
    n = std::min(n, maxOut);
    for (int i = 0; i < n; ++i) out[i] = all[(size_t) i];
    return n;
}

//==============================================================================
// Plain-language engineer notes.
struct LoudnessState { float integrated = -70, shortTerm = -70, momentary = -70, truePeak = -150, lra = 0; bool hasIntegrated = false; };

inline std::vector<std::string> suggestions(const Comparison& c, const Profile& target, const LoudnessState& L, float lufsTarget, size_t maxItems = 4)
{
    std::vector<std::pair<float, std::string>> items;
    char buf[200];
    if (c.valid)
    {
        for (int r = 0; r < numRanges; ++r)
        {
            const float d = c.rangeDb[(size_t) r];
            if (std::abs(d) < 1.0f) continue;
            int peak = -1; float pv = 0;
            for (int b = 0; b < numBands; ++b)
                if (bandInRange(b, r) && useBand(b) && std::abs(c.devDb[(size_t) b]) > std::abs(pv)) { pv = c.devDb[(size_t) b]; peak = b; }
            const double f = peak >= 0 ? bandCentre(peak) : 1000.0;
            const float move = std::min(6.0f, std::round(std::abs(d) * 2.0f) / 2.0f);
            char fs[16];
            if (f >= 1000.0) std::snprintf(fs, sizeof fs, "%.1f kHz", f / 1000.0); else std::snprintf(fs, sizeof fs, "%.0f Hz", f);
            std::snprintf(buf, sizeof buf, "%s is %.1f dB %s than the target - %s ~%.1f dB around %s",
                          range(r).name, std::abs(d), d > 0 ? "heavier" : "lighter", d > 0 ? "cut" : "boost", move, fs);
            items.push_back({ std::abs(d), buf });
        }
        // Translation: what changes at low volume (Fletcher-Munson in action).
        const float qSub = c.rangePhon[0][0], sSub = c.rangePhon[1][0];
        const float qLow = c.rangePhon[0][1], sLow = c.rangePhon[1][1];
        if (c.rangeDb[0] < -1.0f && qSub < -3.0f && qSub < sSub - 1.5f)
        {
            std::snprintf(buf, sizeof buf, "At phone/quiet volume your sub sounds %.1f dB thinner than the target - add upper harmonics (saturation ~100-200 Hz) so the 808 reads small", -qSub);
            items.push_back({ 2.5f + std::abs(qSub) * 0.3f, buf });
        }
        else if (c.rangeDb[1] < -1.0f && qLow < -3.0f && qLow < sLow - 1.5f)
        {
            std::snprintf(buf, sizeof buf, "At quiet volume your low end sounds %.1f dB thinner than the target - check the bass/kick at low level", -qLow);
            items.push_back({ 2.0f + std::abs(qLow) * 0.3f, buf });
        }
    }
    if (L.hasIntegrated)
    {
        const float dl = L.integrated - lufsTarget;
        if (std::abs(dl) >= 1.0f)
        {
            std::snprintf(buf, sizeof buf, "Integrated %.1f LUFS is %.1f LU %s target (%.1f)", L.integrated, std::abs(dl), dl < 0 ? "under" : "over", lufsTarget);
            items.push_back({ 1.5f + std::abs(dl) * 0.2f, buf });
        }
        const float plr = L.truePeak - L.integrated;
        if (L.truePeak > -100.0f && plr < target.plr - 2.0f)
        {
            std::snprintf(buf, sizeof buf, "PLR %.1f dB vs %.1f on the target - it's squashed; ease the limiter or clipper", plr, target.plr);
            items.push_back({ 1.8f, buf });
        }
        if (L.truePeak > -1.0f)
        {
            std::snprintf(buf, sizeof buf, "True peak %.1f dBTP - streaming encoders can clip above -1 dBTP", L.truePeak);
            items.push_back({ 1.2f, buf });
        }
    }
    std::stable_sort(items.begin(), items.end(), [](auto& a, auto& b) { return a.first > b.first; });
    std::vector<std::string> out;
    for (auto& i : items) { if (out.size() >= maxItems) break; out.push_back(i.second); }
    if (out.empty() && c.valid) out.push_back("Within 1 dB of the target in every range - this mix sits with the hits");
    return out;
}

//==============================================================================
// RBJ biquad, transposed direct form II.
struct Biquad
{
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    double z1 = 0, z2 = 0;
    inline float process(float x) { const double y = b0 * x + z1; z1 = b1 * x - a1 * y + z2; z2 = b2 * x - a2 * y; return (float) y; }
    void reset() { z1 = z2 = 0; }
    void setPeak(double sr, double f, double q, double gainDb)
    {
        const double A = std::pow(10.0, gainDb / 40.0), w = 2.0 * pi * f / sr, al = std::sin(w) / (2.0 * q), cs = std::cos(w);
        const double a0 = 1 + al / A;
        b0 = (1 + al * A) / a0; b1 = -2 * cs / a0; b2 = (1 - al * A) / a0; a1 = -2 * cs / a0; a2 = (1 - al / A) / a0;
    }
    double magDb(double sr, double f) const
    {
        const std::complex<double> z = std::polar(1.0, -2.0 * pi * f / sr);
        const auto num = b0 + b1 * z + b2 * z * z, den = 1.0 + a1 * z + a2 * z * z;
        return 20.0 * std::log10(std::abs(num / den));
    }
};

// 10 octave bands 31.5 Hz .. 16 kHz, zero latency. setTargets() solves the band gains so the
// overlapping peaks add up to the requested gain at each centre (no piling up between bands).
class OctaveEq
{
public:
    static constexpr int numBandsEq = 10;
    static double centre(int i) { return 31.25 * std::pow(2.0, i); }
    static constexpr double q = 1.41;

    void prepare(double sampleRate, int channels)
    {
        sr = sampleRate;
        nch = std::min(channels, 2);
        gains.fill(0.0); current.fill(0.0); target.fill(0.0);
        for (auto& ch : f) for (auto& b : ch) { b = Biquad(); }
        dirty = true;
    }
    // Desired response (dB) at each centre -> solved peak gains (message thread is fine; no alloc).
    static std::array<double, numBandsEq> solve(const std::array<double, numBandsEq>& want, double sampleRate)
    {
        std::array<double, numBandsEq> g = want;
        for (int it = 0; it < 12; ++it)
        {
            std::array<Biquad, numBandsEq> bq;
            for (int i = 0; i < numBandsEq; ++i) bq[(size_t) i].setPeak(sampleRate, safeF(i, sampleRate), q, g[(size_t) i]);
            for (int i = 0; i < numBandsEq; ++i)
            {
                double resp = 0;
                for (int j = 0; j < numBandsEq; ++j) resp += bq[(size_t) j].magDb(sampleRate, safeF(i, sampleRate));
                g[(size_t) i] = std::clamp(g[(size_t) i] + 0.7 * (want[(size_t) i] - resp), -18.0, 18.0);
            }
        }
        return g;
    }
    static double safeF(int i, double sampleRate) { return std::min(centre(i), sampleRate * 0.42); }

    // Audio thread: call once per block with the solved gains (smoothly approached, 0.25 dB/block max).
    void setGains(const std::array<double, numBandsEq>& g) { target = g; }

    void process(float* const* ch, int numCh, int num)
    {
        bool changed = false;
        for (int i = 0; i < numBandsEq; ++i)
        {
            const double d = target[(size_t) i] - current[(size_t) i];
            if (std::abs(d) > 1e-4) { current[(size_t) i] += std::clamp(d, -0.25, 0.25); changed = true; }
        }
        if (changed || dirty)
        {
            dirty = false;
            active = false;
            for (int i = 0; i < numBandsEq; ++i)
            {
                if (std::abs(current[(size_t) i]) > 1e-3) active = true;
                for (int c = 0; c < 2; ++c)
                {
                    auto& b = f[(size_t) c][(size_t) i];
                    const double z1 = b.z1, z2 = b.z2;
                    b.setPeak(sr, safeF(i, sr), q, current[(size_t) i]);
                    b.z1 = z1; b.z2 = z2;
                }
            }
        }
        if (! active) return;
        for (int c = 0; c < std::min(numCh, nch); ++c)
        {
            auto* x = ch[c];
            auto& bank = f[(size_t) c];
            for (int n = 0; n < num; ++n)
            {
                float s = x[n];
                for (auto& b : bank) s = b.process(s);
                x[n] = s;
            }
        }
    }
    double responseDb(double hz) const
    {
        double r = 0;
        for (int i = 0; i < numBandsEq; ++i) { Biquad b; b.setPeak(sr, safeF(i, sr), q, current[(size_t) i]); r += b.magDb(sr, hz); }
        return r;
    }
    bool isActive() const { return active; }

private:
    double sr = 48000.0;
    int nch = 2;
    std::array<std::array<Biquad, numBandsEq>, 2> f {};
    std::array<double, numBandsEq> gains {}, current {}, target {};
    bool dirty = true, active = false;
};

// "Hear at": EQ that makes your monitors at `monitorPhon` sound like the same mix heard at `simPhon`
// (loudness-matched at 1 kHz) - the difference of the two ISO 226 contours, per octave centre.
inline std::array<double, OctaveEq::numBandsEq> earSimCurve(double monitorPhon, double simPhon)
{
    std::array<double, OctaveEq::numBandsEq> w {};
    for (int i = 0; i < OctaveEq::numBandsEq; ++i)
    {
        const double f = OctaveEq::centre(i);
        w[(size_t) i] = std::clamp(iso226::earResponse(f, simPhon) - iso226::earResponse(f, monitorPhon), -24.0, 24.0);
    }
    return w;
}

// Match: the correction (dB) at each octave centre that moves the mix's average toward the target.
inline std::array<double, OctaveEq::numBandsEq> matchCurve(const Comparison& c, double maxDb = 6.0)
{
    std::array<double, OctaveEq::numBandsEq> w {};
    if (! c.valid) return w;
    for (int i = 0; i < OctaveEq::numBandsEq; ++i)
    {
        const double f = OctaveEq::centre(i);
        double s = 0, k = 0;
        for (int b = 0; b < numBands; ++b)
        {
            const double o = std::abs(std::log2(bandCentre(b) / f));
            if (o <= 0.55 && useBand(b)) { const double wt = 1.0 - o; s += -c.devDb[(size_t) b] * wt; k += wt; }
        }
        w[(size_t) i] = k > 0 ? std::clamp(s / k, -maxDb, maxDb) : 0.0;
    }
    return w;
}

//==============================================================================
// ITU-R BS.1770-4 loudness + 4x oversampled true peak. Fixed memory.
class LoudnessMeter
{
public:
    void prepare(double sampleRate, int channels)
    {
        sr = sampleRate; nch = std::min(2, std::max(1, channels));
        // K-weighting, coefficients re-derived for any sample rate (same as libebur128).
        {
            const double f0 = 1681.974450955533, G = 3.999843853973347, Q = 0.7071752369554196;
            const double K = std::tan(pi * f0 / sr), Vh = std::pow(10.0, G / 20.0), Vb = std::pow(Vh, 0.4996667741545416);
            const double a0 = 1.0 + K / Q + K * K;
            shelf.b0 = (Vh + Vb * K / Q + K * K) / a0; shelf.b1 = 2.0 * (K * K - Vh) / a0; shelf.b2 = (Vh - Vb * K / Q + K * K) / a0;
            shelf.a1 = 2.0 * (K * K - 1.0) / a0; shelf.a2 = (1.0 - K / Q + K * K) / a0;
        }
        {
            const double f0 = 38.13547087602444, Q = 0.5003270373238773;
            const double K = std::tan(pi * f0 / sr), a0 = 1.0 + K / Q + K * K;
            hp.b0 = 1.0; hp.b1 = -2.0; hp.b2 = 1.0;
            hp.a1 = 2.0 * (K * K - 1.0) / a0; hp.a2 = (1.0 - K / Q + K * K) / a0;
        }
        blockLen = std::max(1, (int) std::lround(sr * 0.1));
        // True-peak interpolator: 4 phases x 12 taps, Hann-windowed sinc.
        for (int ph = 0; ph < 4; ++ph)
            for (int t = 0; t < tapsPerPhase; ++t)
            {
                const double n = t * 4 + ph - (tapsPerPhase * 4 - 1) / 2.0;
                const double x = n / 4.0;
                const double sinc = std::abs(x) < 1e-9 ? 1.0 : std::sin(pi * x) / (pi * x);
                const double w = 0.5 + 0.5 * std::cos(pi * n / (tapsPerPhase * 2.0));
                coef[(size_t) ph][(size_t) t] = (float) (sinc * w);
            }
        reset();
    }
    void reset()
    {
        for (auto& c : st) { c.s = shelf; c.h = hp; c.s.reset(); c.h.reset(); c.hist.fill(0.0f); c.pos = 0; }
        acc = 0; accN = 0;
        ring.fill(0.0); ringPos = 0; blocks = 0;
        hist.fill(0); histP.fill(0.0);
        peak = 0.0f;
        stHist.fill(-70.0f); stCount = 0; stPos = 0;
    }
    void resetIntegrated() { hist.fill(0); histP.fill(0.0); peak = 0.0f; stCount = 0; stPos = 0; }

    void process(const float* const* ch, int numCh, int num)
    {
        const int c = std::min(numCh, nch);
        for (int n = 0; n < num; ++n)
        {
            double e = 0;
            for (int k = 0; k < c; ++k)
            {
                const float x = ch[k][n];
                auto& s = st[(size_t) k];
                const float y = s.h.process(s.s.process(x));
                e += (double) y * y;
                // true peak
                s.hist[(size_t) s.pos] = x;
                s.pos = (s.pos + 1) % tapsPerPhase;
                for (int ph = 0; ph < 4; ++ph)
                {
                    float v = 0;
                    for (int t = 0; t < tapsPerPhase; ++t) v += coef[(size_t) ph][(size_t) t] * s.hist[(size_t) ((s.pos + tapsPerPhase - 1 - t) % tapsPerPhase)];
                    peak = std::max(peak, std::abs(v));
                }
            }
            if (c == 1) e *= 2.0;            // mono plays from both speakers (BS.1770 dual-mono)
            acc += e;
            if (++accN >= blockLen) endBlock();
        }
    }

    LoudnessState state() const
    {
        LoudnessState s;
        s.momentary = blocks >= 4 ? lufsOfMean(4) : -70.0f;
        s.shortTerm = blocks >= 30 ? lufsOfMean(30) : (blocks >= 4 ? lufsOfMean((int) std::min<long long>(blocks, 30)) : -70.0f);
        s.truePeak = peak > 0 ? 20.0f * std::log10(peak) : -150.0f;
        s.hasIntegrated = integrated(s.integrated);
        s.lra = loudnessRange();
        return s;
    }

private:
    struct Ch { Biquad s, h; std::array<float, 12> hist {}; int pos = 0; };
    static constexpr int tapsPerPhase = 12;
    double sr = 48000.0;
    int nch = 2, blockLen = 4800, accN = 0;
    double acc = 0;
    Biquad shelf, hp;
    std::array<Ch, 2> st {};
    std::array<std::array<float, tapsPerPhase>, 4> coef {};
    std::array<double, 30> ring {};          // last 30 x 100 ms block energies
    int ringPos = 0;
    long long blocks = 0;
    // gating histogram of 400 ms blocks: 0.1 LU bins from -70 to +5 LUFS
    static constexpr int histBins = 750;
    std::array<uint32_t, histBins> hist {};
    std::array<double, histBins> histP {};
    float peak = 0.0f;
    // short-term history (one per second) for LRA
    static constexpr int stMax = 3600;
    std::array<float, stMax> stHist {};
    int stCount = 0, stPos = 0, secCounter = 0;

    static float lufs(double meanSq) { return meanSq > 1e-20 ? (float) (-0.691 + 10.0 * std::log10(meanSq)) : -70.0f; }
    float lufsOfMean(int k) const
    {
        double s = 0;
        for (int i = 0; i < k; ++i) s += ring[(size_t) ((ringPos - 1 - i + 300) % 30)];
        return lufs(s / k);
    }
    void endBlock()
    {
        ring[(size_t) ringPos] = acc / accN;
        ringPos = (ringPos + 1) % 30;
        ++blocks;
        acc = 0; accN = 0;
        if (blocks >= 4)
        {
            double s = 0;
            for (int i = 0; i < 4; ++i) s += ring[(size_t) ((ringPos - 1 - i + 300) % 30)];
            const double m = s / 4.0;
            const float l = lufs(m);
            if (l > -70.0f)
            {
                const int bin = std::clamp((int) ((l + 70.0f) * 10.0f), 0, histBins - 1);
                ++hist[(size_t) bin]; histP[(size_t) bin] += m;
            }
        }
        if (++secCounter >= 10 && blocks >= 30)
        {
            secCounter = 0;
            stHist[(size_t) stPos] = lufsOfMean(30);
            stPos = (stPos + 1) % stMax; stCount = std::min(stCount + 1, stMax);
        }
    }
    bool integrated(float& out) const
    {
        double s = 0; uint64_t n = 0;
        for (int i = 0; i < histBins; ++i) { s += histP[(size_t) i]; n += hist[(size_t) i]; }
        if (n == 0) { out = -70.0f; return false; }
        const float rel = lufs(s / (double) n) - 10.0f;
        double s2 = 0; uint64_t n2 = 0;
        for (int i = 0; i < histBins; ++i)
            if (-70.0f + (i + 0.5f) * 0.1f >= rel) { s2 += histP[(size_t) i]; n2 += hist[(size_t) i]; }
        out = n2 ? lufs(s2 / (double) n2) : -70.0f;
        return n2 > 0;
    }
    float loudnessRange() const
    {
        if (stCount < 3) return 0.0f;
        std::array<float, stMax> v {};
        int n = 0; double e = 0; int ne = 0;
        for (int i = 0; i < stCount; ++i) if (stHist[(size_t) i] > -70.0f) { e += std::pow(10.0, (stHist[(size_t) i] + 0.691) / 10.0); ++ne; }
        if (ne < 3) return 0.0f;
        const float rel = lufs(e / ne) - 20.0f;
        for (int i = 0; i < stCount; ++i) if (stHist[(size_t) i] > -70.0f && stHist[(size_t) i] >= rel) v[(size_t) n++] = stHist[(size_t) i];
        if (n < 3) return 0.0f;
        std::sort(v.begin(), v.begin() + n);
        return v[(size_t) (int) std::floor(0.95 * (n - 1))] - v[(size_t) (int) std::floor(0.10 * (n - 1))];
    }
};

} // namespace phon
