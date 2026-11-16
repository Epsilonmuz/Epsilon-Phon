#include "PluginEditor.h"

using namespace epsilon_ui;
namespace
{
const Palette& pal() { static const Palette p = palette(Tier::Epsilon); return p; }
juce::String fmtHz(double f) { return f >= 1000.0 ? juce::String(f / 1000.0, f >= 10000 ? 0 : 1) + "k" : juce::String((int) std::round(f)); }
juce::String fmtLu(float v, bool valid = true) { return valid && v > -69.9f ? juce::String(v, 1) : juce::String::fromUTF8("\xe2\x80\x94"); }
juce::Colour devColour(float v)
{
    const float a = std::abs(v);
    if (a < 1.0f) return pal().meterOk;
    if (a < 3.0f) return pal().meterHot;
    return pal().meterClip;
}
}

//==============================================================================
// BalanceGraph
juce::Rectangle<float> BalanceGraph::plotArea() const { return getLocalBounds().toFloat().reduced(10.0f).withTrimmedLeft(30.0f).withTrimmedBottom(52.0f).withTrimmedTop(14.0f); }
float BalanceGraph::xFor(double hz, juce::Rectangle<float> r) const
{
    const double t = (std::log(hz) - std::log(20.0)) / (std::log(20000.0) - std::log(20.0));
    return r.getX() + (float) t * r.getWidth();
}
float BalanceGraph::yFor(float db, juce::Rectangle<float> r) const
{
    return juce::jmap(juce::jlimit(yBottom, yTop, db), yTop, yBottom, r.getY(), r.getBottom());
}
float BalanceGraph::shown(int b, float db) const
{
    return view.earView ? db + (float) phon::iso226::earResponse(phon::bandCentre(b), view.listen) : db;
}

void BalanceGraph::setView(const PhonView& v, bool targetChanged)
{
    const bool rebuild = targetChanged || v.earView != view.earView || std::abs(v.listen - view.listen) > 0.01f || ! cache.isValid();
    view = v;
    if (rebuild)
    {
        // Fixed range around the target so the picture doesn't jump.
        float hi = -999, lo = 999;
        for (int b = 0; b < phon::numBands; ++b)
            if (view.cmp.valid) { const float t = shown(b, view.cmp.targetDb[(size_t) b]); hi = std::max(hi, t); lo = std::min(lo, t); }
        if (hi < lo) { hi = 0; lo = -30; }
        yTop = std::ceil((hi + 9.0f) / 6.0f) * 6.0f;
        yBottom = std::floor((lo - 12.0f) / 6.0f) * 6.0f;
        if (yTop - yBottom < 42.0f) yBottom = yTop - 42.0f;
        cache = {};
    }
    repaint();
}

void BalanceGraph::renderCache()
{
    const float scale = 2.0f;
    cache = juce::Image(juce::Image::ARGB, juce::jmax(1, (int) (getWidth() * scale)), juce::jmax(1, (int) (getHeight() * scale)), true, juce::SoftwareImageType());
    juce::Graphics g(cache);
    g.addTransform(juce::AffineTransform::scale(scale));
    controls::drawWell(g, getLocalBounds().toFloat(), pal());
    const auto r = plotArea();
    // grid
    for (double f : { 31.5, 63.0, 125.0, 250.0, 500.0, 1000.0, 2000.0, 4000.0, 8000.0, 16000.0 })
    {
        const float x = xFor(f, r);
        g.setColour(pal().line.withAlpha(0.55f));
        g.drawVerticalLine((int) x, r.getY(), r.getBottom());
        g.setColour(pal().inkMuted);
        g.setFont(caption());
        g.drawText(fmtHz(f), juce::Rectangle<float>(x - 20, r.getBottom() + 3, 40, 14), juce::Justification::centred, false);
    }
    for (float d = yBottom; d <= yTop + 0.01f; d += 6.0f)
    {
        const float y = yFor(d, r);
        g.setColour(pal().line.withAlpha(0.35f));
        g.drawHorizontalLine((int) y, r.getX(), r.getRight());
        g.setColour(faint(pal()));
        g.setFont(font(Weight::Medium, 10.0f));
        g.drawText(juce::String((int) d), juce::Rectangle<float>(r.getX() - 32, y - 7, 28, 14), juce::Justification::centredRight, false);
    }
    // range names along the top
    for (int i = 0; i < phon::numRanges; ++i)
    {
        const auto& rg = phon::range(i);
        const float x0 = xFor(juce::jmax(20.0, rg.lo), r), x1 = xFor(juce::jmin(20000.0, rg.hi), r);
        if (i > 0) { g.setColour(pal().line); g.drawVerticalLine((int) x0, r.getY() - 12, r.getY()); }
        g.setColour(faint(pal()));
        g.setFont(font(Weight::Bold, 9.5f).withExtraKerningFactor(0.18f));
        g.drawText(juce::String(rg.name).toUpperCase(), juce::Rectangle<float>(x0, r.getY() - 13, x1 - x0, 12), juce::Justification::centred, false);
    }
    if (! view.cmp.valid) return;
    // target: tolerance band (+-1.5 dB) + platinum line
    juce::Path band, line;
    for (int b = 0; b < phon::numBands; ++b)
    {
        const float x = xFor(phon::bandCentre(b), r), t = shown(b, view.cmp.targetDb[(size_t) b]);
        if (b == 0) { line.startNewSubPath(x, yFor(t, r)); band.startNewSubPath(x, yFor(t + 1.5f, r)); }
        else { line.lineTo(x, yFor(t, r)); band.lineTo(x, yFor(t + 1.5f, r)); }
    }
    for (int b = phon::numBands - 1; b >= 0; --b) band.lineTo(xFor(phon::bandCentre(b), r), yFor(shown(b, view.cmp.targetDb[(size_t) b]) - 1.5f, r));
    band.closeSubPath();
    g.setColour(pal().trim.withAlpha(0.10f));
    g.fillPath(band);
    g.setColour(pal().trim.withAlpha(0.85f));
    g.strokePath(line.createPathWithRoundedCorners(8.0f), juce::PathStrokeType(1.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    // the ear's own curve at this level (Ear X-ray)
    if (view.earView)
    {
        juce::Path ear;
        for (int i = 0; i <= 120; ++i)
        {
            const double f = 20.0 * std::pow(1000.0, i / 120.0);
            const float y = yFor(yBottom + 4.0f + (float) phon::iso226::earResponse(f, view.listen) * 0.5f + 18.0f, r);
            if (i == 0) ear.startNewSubPath(xFor(f, r), y); else ear.lineTo(xFor(f, r), y);
        }
        juce::Path dashed;
        const float dashes[] = { 4.0f, 4.0f };
        juce::PathStrokeType(1.0f).createDashedStroke(dashed, ear, dashes, 2);
        g.setColour(pal().inkMuted.withAlpha(0.55f));
        g.fillPath(dashed);
        g.setFont(caption());
        g.drawText(juce::String::fromUTF8("YOUR EAR AT ") + juce::String(view.listen, 0) + " dB SPL (ISO 226, \xc2\xbd scale)",
                   juce::Rectangle<float>(r.getRight() - 330, r.getBottom() - 16, 322, 14), juce::Justification::centredRight, false);
    }
}

void BalanceGraph::paint(juce::Graphics& g)
{
    if (! cache.isValid()) renderCache();
    g.drawImage(cache, getLocalBounds().toFloat());
    const auto r = plotArea();
    const auto& c = view.cmp;
    if (! c.valid)
    {
        g.setColour(pal().inkMuted);
        g.setFont(body());
        g.drawText(view.target.valid ? "Play your mix through Epsilon Phon" : "Load hit records to use as the target (drop files here)", r, juce::Justification::centred, false);
        return;
    }
    juce::Graphics::ScopedSaveState ss(g);
    g.reduceClipRegion(r.expanded(2.0f).toNearestInt());
    // deviation fill between the long-term mix and the target
    for (int b = 0; b < phon::numBands - 1; ++b)
    {
        const float x0 = xFor(phon::bandCentre(b), r), x1 = xFor(phon::bandCentre(b + 1), r);
        juce::Path p;
        p.startNewSubPath(x0, yFor(shown(b, c.mixDb[(size_t) b]), r));
        p.lineTo(x1, yFor(shown(b + 1, c.mixDb[(size_t) b + 1]), r));
        p.lineTo(x1, yFor(shown(b + 1, c.targetDb[(size_t) b + 1]), r));
        p.lineTo(x0, yFor(shown(b, c.targetDb[(size_t) b]), r));
        p.closeSubPath();
        const float d = 0.5f * (c.devDb[(size_t) b] + c.devDb[(size_t) b + 1]);
        g.setColour((d > 0 ? pal().meterHot : juce::Colour(0xff7fa8d6)).withAlpha(juce::jlimit(0.05f, 0.32f, std::abs(d) * 0.07f)));
        g.fillPath(p);
    }
    // live (fast) curve, matched with the same offset as the average
    float fastOff = 0; { float s = 0; int n = 0; for (int b = 4; b < 26; ++b) if (view.snap.fast[(size_t) b] > -140) { s += view.snap.fast[(size_t) b] - c.mixDb[(size_t) b]; ++n; } fastOff = n ? s / n : 0; }
    juce::Path live, avg;
    for (int b = 0; b < phon::numBands; ++b)
    {
        const float x = xFor(phon::bandCentre(b), r);
        const float yl = yFor(shown(b, view.snap.fast[(size_t) b] - fastOff), r), ya = yFor(shown(b, c.mixDb[(size_t) b]), r);
        if (b == 0) { live.startNewSubPath(x, yl); avg.startNewSubPath(x, ya); } else { live.lineTo(x, yl); avg.lineTo(x, ya); }
    }
    g.setColour(pal().live.withAlpha(0.5f));
    g.strokePath(live.createPathWithRoundedCorners(8.0f), juce::PathStrokeType(1.0f));
    g.setColour(pal().ink);
    g.strokePath(avg.createPathWithRoundedCorners(8.0f), juce::PathStrokeType(2.2f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    g.restoreState();

    // match EQ strip
    auto strip = getLocalBounds().toFloat().reduced(10.0f).withTrimmedLeft(30.0f).removeFromBottom(30.0f);
    const float mid = strip.getCentreY();
    g.setColour(pal().line);
    g.drawHorizontalLine((int) mid, strip.getX(), strip.getRight());
    g.setColour(view.matchOn ? pal().live : faint(pal()));
    g.setFont(font(Weight::Bold, 9.5f).withExtraKerningFactor(0.18f));
    g.drawText(view.matchOn ? "MATCH EQ" : "MATCH EQ (LEARN TO ARM)", strip.withTrimmedRight(4).translated(0, -2), juce::Justification::topRight, false);
    if (view.matchOn)
    {
        juce::Path m;
        for (int i = 0; i < (int) view.matchResp.size(); ++i)
        {
            const double f = 20.0 * std::pow(1000.0, i / (double) (view.matchResp.size() - 1));
            const float y = mid - juce::jlimit(-6.0f, 6.0f, view.matchResp[(size_t) i]) / 6.0f * (strip.getHeight() * 0.5f - 2.0f);
            if (i == 0) m.startNewSubPath(xFor(f, r), y); else m.lineTo(xFor(f, r), y);
        }
        g.setColour(pal().live);
        g.strokePath(m, juce::PathStrokeType(1.5f));
    }
}

//==============================================================================
// LiveMeter - shares the graph's frequency axis (same left/right insets).
void LiveMeter::update(const phon::LiveDev& d, float dt, bool target)
{
    haveTarget = target;
    tracker.update(d, dt);
    const auto now = juce::Time::getMillisecondCounter();
    if (now - lastText > 250)                       // names refresh 4x a second so they can be read
    {
        lastText = now;
        nOver = phon::offenders(tracker.smooth, 2.5f, true, over.data(), 3);
        nUnder = phon::offenders(tracker.smooth, 3.5f, false, under.data(), 2);
    }
    repaint();
}

void LiveMeter::paint(juce::Graphics& g)
{
    const auto full = getLocalBounds().toFloat();
    controls::drawWell(g, full, pal());
    auto r = full.reduced(10.0f, 6.0f).withTrimmedLeft(30.0f);
    auto textRow = r.removeFromTop(22.0f);
    const float range = 9.0f;
    const float mid = r.getCentreY(), half = r.getHeight() * 0.5f - 1.0f;
    const auto xFor = [&](double hz) { return r.getX() + (float) ((std::log(hz) - std::log(20.0)) / (std::log(20000.0) - std::log(20.0))) * r.getWidth(); };
    const auto yFor = [&](float db) { return mid - juce::jlimit(-range, range, db) / range * half; };

    // tolerance zone +-2 dB and scale
    g.setColour(pal().meterOk.withAlpha(0.07f));
    g.fillRect(juce::Rectangle<float>(r.getX(), yFor(2.0f), r.getWidth(), yFor(-2.0f) - yFor(2.0f)));
    g.setColour(pal().line);
    g.drawHorizontalLine((int) mid, r.getX(), r.getRight());
    g.setFont(font(Weight::Medium, 9.5f));
    g.setColour(faint(pal()));
    g.drawText("+9", juce::Rectangle<float>(full.getX() + 4, r.getY() - 2, 30, 12), juce::Justification::centredRight, false);
    g.drawText("0", juce::Rectangle<float>(full.getX() + 4, mid - 6, 30, 12), juce::Justification::centredRight, false);
    g.drawText("-9", juce::Rectangle<float>(full.getX() + 4, r.getBottom() - 10, 30, 12), juce::Justification::centredRight, false);

    const float bw = (xFor(phon::bandCentre(1)) - xFor(phon::bandCentre(0))) * 0.72f;
    for (int b = 0; b < phon::numBands; ++b)
    {
        if (! phon::useBand(b)) continue;
        const float x = xFor(phon::bandCentre(b)), v = tracker.inst[(size_t) b];
        juce::Colour c = v > 4.0f ? pal().meterClip : v > 2.0f ? pal().meterHot : v < -2.0f ? juce::Colour(0xff7fa8d6) : pal().meterOk.withAlpha(0.55f);
        if (! tracker.active) c = pal().raised;
        const float y = yFor(v);
        g.setColour(c);
        g.fillRoundedRectangle(juce::Rectangle<float>(x - bw * 0.5f, std::min(y, mid), bw, std::max(1.5f, std::abs(y - mid))), 1.5f);
        const float h = tracker.hold[(size_t) b];
        if (h > 2.0f)
        {
            g.setColour((h > 4.0f ? pal().meterClip : pal().meterHot).withAlpha(0.9f));
            g.fillRect(juce::Rectangle<float>(x - bw * 0.5f, yFor(h) - 1.0f, bw, 2.0f));
        }
    }

    // call-outs: what is too loud right now, in words
    g.setFont(font(Weight::Bold, 9.5f).withExtraKerningFactor(0.16f));
    g.setColour(pal().inkMuted);
    float x = textRow.getX();
    const auto label = [&](const juce::String& t, juce::Colour c)
    {
        g.setFont(font(Weight::Bold, 9.5f).withExtraKerningFactor(0.16f));
        g.setColour(c);
        const float w = (float) juce::GlyphArrangement::getStringWidthInt(g.getCurrentFont(), t) + 10.0f;
        g.drawText(t, juce::Rectangle<float>(x, textRow.getY(), w, 18.0f), juce::Justification::centredLeft, false);
        x += w;
    };
    const auto chip = [&](const phon::Offender& o, juce::Colour c)
    {
        const double f = phon::bandCentre(o.band);
        const juce::String t = (f >= 1000.0 ? juce::String(f / 1000.0, 1) + " kHz" : juce::String((int) std::round(f)) + " Hz")
                             + "  " + (o.db > 0 ? "+" : "") + juce::String(o.db, 1) + " dB";
        g.setFont(font(Weight::Bold, 11.5f));
        const float w = (float) juce::GlyphArrangement::getStringWidthInt(g.getCurrentFont(), t) + 16.0f;
        const juce::Rectangle<float> box(x, textRow.getY(), w, 18.0f);
        g.setColour(c.withAlpha(0.18f)); g.fillRoundedRectangle(box, 5.0f);
        g.setColour(c.withAlpha(0.7f)); g.drawRoundedRectangle(box.reduced(0.5f), 5.0f, 1.0f);
        g.setColour(pal().ink);
        g.drawText(t, box, juce::Justification::centred, false);
        x += w + 6.0f;
    };
    g.setFont(font(Weight::Bold, 9.0f).withExtraKerningFactor(0.14f));
    g.setColour(faint(pal()));
    g.drawText("LIVE METER  -  UP = TOO LOUD  -  DOWN = MISSING", juce::Rectangle<float>(r.getRight() - 320.0f, r.getBottom() - 12.0f, 320.0f, 12.0f), juce::Justification::centredRight, false);
    if (! haveTarget) label("LIVE  -  LOAD HITS OR PICK A GENRE TO JUDGE AGAINST", faint(pal()));
    else if (! tracker.active) label("LIVE  -  WAITING FOR AUDIO", faint(pal()));
    else
    {
        if (nOver == 0 && nUnder == 0) label("LIVE  -  NOTHING STICKING OUT RIGHT NOW", pal().meterOk);
        if (nOver > 0)
        {
            label("TOO LOUD NOW", pal().meterHot);
            for (int i = 0; i < nOver; ++i) chip(over[(size_t) i], over[(size_t) i].db > 4.0f ? pal().meterClip : pal().meterHot);
            x += 10.0f;
        }
        if (nUnder > 0)
        {
            label("MISSING", juce::Colour(0xff7fa8d6));
            for (int i = 0; i < nUnder; ++i) chip(under[(size_t) i], juce::Colour(0xff7fa8d6));
        }
    }
}

//==============================================================================
void TranslationGrid::paint(juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat();
    const auto& c = view.cmp;
    // score
    auto scoreArea = r.removeFromLeft(86.0f);
    g.setColour(c.valid ? pal().live : faint(pal()));
    g.setFont(font(Weight::Bold, 38.0f));
    g.drawText(c.valid ? juce::String((int) std::round(c.score)) : juce::String::fromUTF8("\xe2\x80\x94"), scoreArea.withHeight(60).translated(0, 22), juce::Justification::centred, false);
    g.setColour(pal().inkMuted);
    g.setFont(font(Weight::Bold, 9.0f).withExtraKerningFactor(0.18f));
    g.drawText("HIT MATCH", scoreArea.withHeight(14).translated(0, 84), juce::Justification::centred, false);
    g.setFont(caption());
    g.drawText("/ 100", scoreArea.withHeight(14).translated(0, 98), juce::Justification::centred, false);

    r.removeFromLeft(6.0f);
    const float labelW = 62.0f;
    auto head = r.removeFromTop(18.0f);
    head.removeFromLeft(labelW);
    const float cw = head.getWidth() / phon::numRanges;
    g.setFont(font(Weight::Bold, 9.0f).withExtraKerningFactor(0.12f));
    for (int i = 0; i < phon::numRanges; ++i)
    {
        g.setColour(pal().inkMuted);
        static const char* shortName[] = { "SUB", "LOW", "LO-MID", "HI-MID", "HIGH" };
        g.drawText(shortName[i], juce::Rectangle<float>(head.getX() + i * cw, head.getY(), cw, head.getHeight()), juce::Justification::centred, false);
    }
    const char* rows[3] = { "PHONE", "YOU", "LOUD" };
    const float rh = juce::jmin(36.0f, r.getHeight() / 3.0f);
    for (int L = 0; L < 3; ++L)
    {
        auto row = r.removeFromTop(rh);
        auto lab = row.removeFromLeft(labelW);
        g.setColour(L == 1 ? pal().ink : pal().inkMuted);
        g.setFont(font(Weight::Bold, 9.5f).withExtraKerningFactor(0.14f));
        g.drawText(rows[L], lab.withTrimmedBottom(lab.getHeight() * 0.45f), juce::Justification::bottomLeft, false);
        g.setColour(faint(pal()));
        g.setFont(font(Weight::Medium, 10.0f));
        g.drawText(juce::String(c.levels[(size_t) L], 0) + " dB", lab.withTrimmedTop(lab.getHeight() * 0.55f), juce::Justification::topLeft, false);
        for (int i = 0; i < phon::numRanges; ++i)
        {
            auto cell = juce::Rectangle<float>(row.getX() + i * cw, row.getY(), cw, row.getHeight()).reduced(2.5f);
            const float v = c.valid ? c.rangePhon[(size_t) L][(size_t) i] : 0.0f;
            const auto col = devColour(v);
            g.setColour(c.valid ? col.withAlpha(0.16f) : pal().raised);
            g.fillRoundedRectangle(cell, 5.0f);
            g.setColour(c.valid ? col.withAlpha(0.55f) : pal().line);
            g.drawRoundedRectangle(cell.reduced(0.5f), 5.0f, 1.0f);
            g.setColour(c.valid ? pal().ink : faint(pal()));
            g.setFont(font(Weight::Bold, 12.5f));
            g.drawText(c.valid ? ((v >= 0 ? "+" : "") + juce::String(v, 1)) : juce::String::fromUTF8("\xc2\xb7"), cell, juce::Justification::centred, false);
        }
    }
    g.setColour(faint(pal()));
    g.setFont(font(Weight::Medium, 10.0f));
    g.drawText("perceived dB vs target (sones, ISO 226 + ISO 532)", r.withHeight(14).translated(0, 4), juce::Justification::centredLeft, false);
}

//==============================================================================
void LoudnessView::set(const phon::LoudnessState& l, float t, const juce::String& n)
{
    loud = l; target = t; targetName = n;
    repaint();
}

void LoudnessView::paint(juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat();
    // hero: integrated (what streaming services read)
    auto hero = r.removeFromTop(70.0f);
    g.setColour(loud.hasIntegrated ? pal().ink : faint(pal()));
    g.setFont(font(Weight::Bold, 40.0f));
    g.drawText(fmtLu(loud.integrated, loud.hasIntegrated), hero.removeFromLeft(140.0f), juce::Justification::centredLeft, false);
    g.setColour(pal().inkMuted);
    g.setFont(font(Weight::Bold, 10.0f).withExtraKerningFactor(0.16f));
    g.drawText("LUFS", hero.removeFromTop(38.0f), juce::Justification::bottomLeft, false);
    g.setFont(caption());
    g.drawText("integrated", hero, juce::Justification::topLeft, false);

    // bar: -24 .. -4 LUFS with the target tick
    auto bar = r.removeFromTop(30.0f).withTrimmedTop(6.0f).withHeight(12.0f).reduced(12.0f, 0.0f);
    g.setColour(pal().bg);
    g.fillRoundedRectangle(bar, 6.0f);
    const auto xOf = [&](float v) { return juce::jmap(juce::jlimit(-24.0f, -4.0f, v), -24.0f, -4.0f, bar.getX(), bar.getRight()); };
    if (loud.shortTerm > -60.0f)
    {
        const float d = loud.shortTerm - target;
        g.setColour(std::abs(d) < 1.0f ? pal().meterOk : d > 0 ? pal().meterHot : pal().live.withAlpha(0.8f));
        g.fillRoundedRectangle(bar.withRight(xOf(loud.shortTerm)), 6.0f);
    }
    g.setColour(pal().ink);
    const float tx = xOf(target);
    g.fillRect(tx - 1.0f, bar.getY() - 4.0f, 2.0f, bar.getHeight() + 8.0f);
    g.setFont(font(Weight::Medium, 9.5f));
    g.setColour(faint(pal()));
    for (float v : { -24.0f, -18.0f, -14.0f, -9.0f, -4.0f })
        g.drawText(juce::String((int) v), juce::Rectangle<float>(xOf(v) - 14, bar.getBottom() + 2, 28, 12), juce::Justification::centred, false);

    r.removeFromTop(16.0f);
    const auto row = [&](const juce::String& k, const juce::String& v, juce::Colour c)
    {
        auto line = r.removeFromTop(22.0f);
        g.setColour(pal().inkMuted);
        g.setFont(font(Weight::Bold, 10.0f).withExtraKerningFactor(0.14f));
        g.drawText(k.toUpperCase(), line, juce::Justification::centredLeft, false);
        g.setColour(c);
        g.setFont(font(Weight::Bold, 14.0f));
        g.drawText(v, line, juce::Justification::centredRight, false);
    };
    const float plr = loud.truePeak - loud.integrated;
    row("Short-term", fmtLu(loud.shortTerm) + " LUFS", pal().ink);
    row("Momentary", fmtLu(loud.momentary) + " LUFS", pal().inkMuted);
    row("True peak", loud.truePeak > -100 ? juce::String(loud.truePeak, 1) + " dBTP" : juce::String::fromUTF8("\xe2\x80\x94"),
        loud.truePeak > -1.0f ? pal().meterClip : pal().ink);
    row("PLR", loud.hasIntegrated && loud.truePeak > -100 ? juce::String(plr, 1) + " dB" : juce::String::fromUTF8("\xe2\x80\x94"), pal().ink);
    row("LRA", loud.lra > 0 ? juce::String(loud.lra, 1) + " LU" : juce::String::fromUTF8("\xe2\x80\x94"), pal().inkMuted);
    const float d = loud.integrated - target;
    row("Target  " + targetName, juce::String(target, 1) + " LUFS", pal().trim);
    row("To target", loud.hasIntegrated ? ((-d > 0 ? "+" : "") + juce::String(-d, 1) + " LU") : juce::String::fromUTF8("\xe2\x80\x94"),
        loud.hasIntegrated ? (std::abs(d) < 1.0f ? pal().meterOk : pal().meterHot) : faint(pal()));
}

//==============================================================================
void NotesView::set(const std::vector<std::string>& n, const juce::String& s)
{
    if (n == lines && s == status) return;
    lines = n; status = s;
    repaint();
}
void NotesView::paint(juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat();
    auto st = r.removeFromBottom(18.0f);
    g.setColour(faint(pal()));
    g.setFont(caption());
    g.drawText(status, st, juce::Justification::centredLeft, true);
    for (size_t i = 0; i < lines.size(); ++i)
    {
        juce::AttributedString a;
        a.append(juce::String::fromUTF8(lines[i].c_str()), font(Weight::Medium, 12.5f), i == 0 ? pal().ink : pal().inkMuted);
        a.setWordWrap(juce::AttributedString::byWord);
        juce::TextLayout tl;
        tl.createLayout(a, r.getWidth() - 16.0f);
        const float h = juce::jmin(tl.getHeight(), 48.0f);
        if (r.getHeight() < h) break;
        auto item = r.removeFromTop(h + 8.0f);
        g.setColour(i == 0 ? pal().live : pal().trim.withAlpha(0.7f));
        g.fillRoundedRectangle(item.getX(), item.getY() + 5.0f, 8.0f, 2.5f, 0.5f);
        tl.draw(g, item.withTrimmedLeft(16.0f).withHeight(h));
    }
}

//==============================================================================
// HelpOverlay
HelpOverlay::HelpOverlay()
{
    setOpaque(true);
    viewport.setViewedComponent(&content, false);
    viewport.setScrollBarsShown(true, false);
    viewport.setScrollBarThickness(10);
    viewport.getVerticalScrollBar().setColour(juce::ScrollBar::thumbColourId, pal().trim.withAlpha(0.6f));
    addAndMakeVisible(viewport);
    addAndMakeVisible(closeButton);
    closeButton.onClick = [this] { if (onClose) onClose(); };
}
void HelpOverlay::setSections(std::vector<Section> s) { content.sections = std::move(s); resized(); }
void HelpOverlay::resized()
{
    auto r = getLocalBounds().reduced(size::space4);
    auto top = r.removeFromTop(34);
    closeButton.setBounds(top.removeFromRight(90).withSizeKeepingCentre(90, 28));
    r.removeFromTop(6);
    viewport.setBounds(r);
    const int w = r.getWidth() - viewport.getScrollBarThickness() - 4;
    content.setSize(w, content.layout(w));
}
void HelpOverlay::paint(juce::Graphics& g)
{
    g.fillAll(pal().bg);
    auto r = getLocalBounds().reduced(size::space4).removeFromTop(34).toFloat();
    drawSectionLabel(g, { r.getX() + 2.0f, r.getCentreY() }, "Epsilon Phon manual", pal());
    g.setColour(faint(pal()));
    g.setFont(caption());
    g.drawText("Scroll for more. Hover any control in the plugin to read what it does in the bottom bar.", r.withTrimmedLeft(210.0f).withTrimmedRight(100.0f), juce::Justification::centredLeft, true);
}
int HelpOverlay::Content::layout(int width)
{
    rows.clear();
    const float nameW = 170.0f, pad = 16.0f;
    float y = 4.0f;
    for (auto& sec : sections)
    {
        Row h; h.heading = true; h.name = sec.title; h.y = y + 10.0f; h.h = 30.0f;
        rows.push_back(std::move(h));
        y += 44.0f;
        for (auto& it : sec.items)
        {
            Row r; r.name = it.name;
            juce::AttributedString a;
            a.append(it.text, font(Weight::Medium, 13.5f), pal().ink.withAlpha(0.86f));
            a.setWordWrap(juce::AttributedString::byWord);
            a.setLineSpacing(3.0f);
            r.text.createLayout(a, (float) width - nameW - pad * 2.0f);
            r.y = y; r.h = juce::jmax(20.0f, r.text.getHeight());
            y += r.h + 12.0f;
            rows.push_back(std::move(r));
        }
        y += 8.0f;
    }
    return (int) y + 12;
}
void HelpOverlay::Content::paint(juce::Graphics& g)
{
    const float nameW = 170.0f, pad = 16.0f;
    const auto clip = g.getClipBounds().toFloat();
    for (auto& r : rows)
    {
        if (r.y > clip.getBottom() || r.y + r.h + 20.0f < clip.getY()) continue;
        if (r.heading)
        {
            g.setColour(pal().surface);
            g.fillRoundedRectangle(0.0f, r.y, (float) getWidth(), r.h, 6.0f);
            g.setColour(pal().live);
            g.fillRoundedRectangle(pad, r.y + r.h * 0.5f - 1.25f, 10.0f, 2.5f, 0.5f);
            g.setColour(pal().ink);
            g.setFont(font(Weight::Bold, 12.0f).withExtraKerningFactor(0.18f));
            g.drawText(r.name.toUpperCase(), juce::Rectangle<float>(pad + 18.0f, r.y, (float) getWidth() - 60.0f, r.h), juce::Justification::centredLeft, false);
        }
        else
        {
            g.setColour(pal().trim);
            g.setFont(font(Weight::Bold, 12.5f));
            g.drawFittedText(r.name, juce::Rectangle<float>(pad, r.y, nameW - 12.0f, 34.0f).toNearestInt(), juce::Justification::topLeft, 2);
            r.text.draw(g, juce::Rectangle<float>(pad + nameW, r.y, (float) getWidth() - nameW - pad * 2.0f, r.h));
        }
    }
}

//==============================================================================
void EpsilonPhonEditor::describe(juce::Component& c, const juce::String& name, const juce::String& text)
{
    helpEntries.push_back({ &c, name, text });
    if (auto* k = dynamic_cast<epsilon_ui::Knob*>(&c)) k->setTooltip(text);
    else if (auto* t = dynamic_cast<juce::SettableTooltipClient*>(&c)) t->setTooltip(text);
}

std::vector<HelpOverlay::Section> EpsilonPhonEditor::manual() const
{
    std::vector<HelpOverlay::Section> m;
    m.push_back({ "Quick start", {
        { "1  Place it", "Put Epsilon Phon LAST on your master channel, after your limiter. It adds no latency." },
        { "2  Set the standard", "Press LOAD HITS and choose 1-10 finished hit records in your exact style (or drag them onto the plugin). They are averaged into one target. No records handy? Pick a genre in TARGET." },
        { "3  Set your volume", "Turn LISTENING LEVEL to how loud your speakers really are at your seat. Don't know? Leave it at 79 dB (small room)." },
        { "4  Play the whole song", "Watch the LIVE METER: anything amber or red is louder than it should be right now, and it is named in words (for example TOO LOUD NOW 251 Hz +4.2 dB). Blue means missing." },
        { "5  Fix and check", "Read ENGINEER NOTES for the biggest fixes, fix them in your mix, press RESET and play again. HIT MATCH climbing toward 100 means you are closing in on the hits." } } });

    HelpOverlay::Section controls { "Every button and knob", {} };
    for (auto& e : helpEntries) controls.items.push_back({ e.name, e.text });
    m.push_back(std::move(controls));

    m.push_back({ "Reading the loudness numbers", {
        { "Integrated LUFS", "The loudness of everything since you pressed Reset - the one number streaming services measure. Play the full song to get a true reading." },
        { "Short-term", "Loudness of the last 3 seconds. Tells you how loud the current section (hook, verse) is. This is what the bar shows." },
        { "Momentary", "Loudness of the last 0.4 seconds. Follows individual hits and words." },
        { "True peak (dBTP)", "The highest peak, including peaks between samples. Turns red above -1 dBTP: streaming encoders can distort above that." },
        { "PLR", "Peak-to-loudness ratio: true peak minus integrated. How much punch is left. Around 9 dB is typical for current hits; much less means it is squashed." },
        { "LRA", "Loudness range: how much the loudness moves between quiet and loud sections." },
        { "Target / To target", "The loudness goal and how many LU you must go up (+) or down (-) to reach it." },
        { "White tick on the bar", "The loudness goal. The bar turns green when the current section is within 1 LU of it." } } });

    m.push_back({ "Colours and words", {
        { "Amber / red", "Louder than the target. Amber = 2-4 dB over, red = more than 4 dB over." },
        { "Blue", "Quieter than the target (missing)." },
        { "Green", "Within tolerance. In the live meter the green zone is +-2 dB." },
        { "Platinum line", "The target (your hits or the genre curve), shown at the same loudness as your mix. The soft band around it is +-1.5 dB." },
        { "White line", "Your mix's average since Reset. The thin champagne line is the last fraction of a second." },
        { "Sub, Low, Low-mid, High-mid, High", "The five ranges: under 63 Hz, 63-250 Hz, 250 Hz-1 kHz, 1-5 kHz, above 5 kHz." },
        { "Phon / sones", "Phon is how loud something sounds, not how loud it measures. Sones are the same idea on a scale where double the number sounds twice as loud. The Translation numbers use them." } } });

    m.push_back({ "Why volume changes your mix (Fletcher-Munson)", {
        { "The effect", "Your ears hear bass and extreme treble worse at low volume than at high volume. The same mix sounds thin when quiet and bass-heavy when loud. The modern measurement of this is the ISO 226 standard, which this plugin uses." },
        { "What goes wrong", "Mix quietly and you add too much bass and top. Mix loud and you add too little. Either way the mix falls apart on other systems." },
        { "What to do", "Pick one moderate monitor level and stay there (about 79 dB SPL in a small room, 83-85 in a large one), compare against real hits at the same loudness, and check phone and loud playback with HEAR AT and the TRANSLATION grid." },
        { "Calibrating", "Play pink noise at -20 dBFS RMS, hold a sound-level meter (C-weighted, slow) at your seat, and set your speaker volume until it reads your chosen level. Mark that position on the volume knob." } } });

    m.push_back({ "Good to know", {
        { "Before you bounce", "Set HEAR AT to OFF - it is for listening only and would be printed into your file. The bottom bar warns you while it is on. MATCH is printed on purpose: set it to 0 % if you only want to measure." },
        { "Best references", "Use recent, mastered hits in your own lane (dembow against dembow, drill against drill). 3-10 records give a steadier target than one." },
        { "Genre curves", "The built-in genre targets are models shaped from published averages, not measurements of specific songs. Your own loaded hits are always the more accurate standard." },
        { "The live meter jumps", "That is normal: a kick or a vocal note really does poke out for a moment. Worry about bands that stay amber or red, or keep being named." },
        { "Limits", "Matching the hits' balance puts you in the right zone; it does not replace your taste. Some hit records break the rules on purpose." } } });
    return m;
}

void EpsilonPhonEditor::showHelp(bool shouldShow)
{
    if (shouldShow) { help.setSections(manual()); help.toFront(false); }
    help.setVisible(shouldShow);
    helpButton.setToggleState(shouldShow, juce::dontSendNotification);
}

//==============================================================================
EpsilonPhonEditor::EpsilonPhonEditor(EpsilonPhonProcessor& p) : AudioProcessorEditor(&p), proc(p)
{
    setLookAndFeel(&look);
    setOpaque(true);
    addAndMakeVisible(header);
    addAndMakeVisible(footer);
    footer.statusText = [this] { return statusText(); };
    footer.cpuPercent = [this] { return proc.getCpuPercent(); };

    targetBox.addItemList(EpsilonPhonProcessor::targetChoices(), 1);
    targetAtt = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>(proc.state, "target", targetBox);
    loudBox.addItemList({ "Match target", "Streaming -14", "Apple -16", "Club -7" }, 1);
    loudAtt = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>(proc.state, "loudTarget", loudBox);

    loadButton.onClick = [this] { chooseFiles(); };
    resetButton.onClick = [this] { proc.resetAnalysis(); };
    learnButton.onClick = [this] { proc.learnMatch(); };
    clearButton.onClick = [this] { proc.clearMatch(); };
    xrayButton.setClickingTogglesState(true);
    xrayButton.setToggleState((bool) proc.state.state.getProperty("earXray", false), juce::dontSendNotification);
    xrayButton.onClick = [this] { proc.state.state.setProperty("earXray", xrayButton.getToggleState(), nullptr); timerCallback(); };


    helpButton.setClickingTogglesState(true);
    helpButton.onClick = [this] { showHelp(helpButton.getToggleState()); };
    help.onClose = [this] { showHelp(false); };

    // One list feeds the tooltips, the hover-help line in the bottom bar and the manual.
    describe(helpButton, "HELP", "Opens the built-in manual: quick start, every control, how to read the numbers.");
    describe(targetBox, "TARGET", "The standard you are judged against: 'Hit references' = the records you loaded, or a modelled genre curve.");
    describe(loadButton, "LOAD HITS", "Choose 1-10 finished hit records (WAV, AIFF, FLAC, MP3). They are averaged into your target. You can also drag files onto the plugin.");
    describe(resetButton, "RESET", "Starts a fresh measurement: clears the average curve and the integrated loudness. Press it after you change your mix.");
    describe(xrayButton, "EAR X-RAY", "Redraws the graph the way your ears hear it at your listening level (ISO 226), instead of how a meter sees it.");
    describe(graph, "TONAL BALANCE graph", "Bass on the left, treble on the right. White = your mix's average, platinum = the target, thin line = right now. Amber fill = too much, blue = too little.");
    describe(live, "LIVE METER", "Real time: each bar is one frequency band against the target right now. Up amber/red = louder than it should be, down blue = missing. Worst ones are named above the bars.");
    describe(loudBox, "GOAL", "Loudness goal for the meter: 'Match target' = as loud as your hits, or Streaming -14, Apple -16, Club -7 LUFS.");
    describe(loudness, "LOUDNESS panel", "Integrated = whole song (what streaming measures). Short-term = last 3 s. True peak red = above -1 dBTP. PLR = punch left. To target = how far to go.");
    describe(grid, "TRANSLATION grid", "How each range SOUNDS against the hits at phone volume (60 dB), your volume, and loud (90 dB). + = heavier, - = thinner. Green is good. HIT MATCH is the overall score.");
    describe(notes, "ENGINEER NOTES", "The biggest fixes in plain words, most important first. Appear after about 8 seconds of audio.");
    describe(listenKnob, "LISTENING LEVEL", "How loud your speakers are at your seat, in dB SPL. 79 = small room, 83-85 = large room. Sets what Ear X-ray, Translation and Hear At are based on.");
    describe(matchKnob, "MATCH", "How much of the learned Match EQ is applied to your mix. This IS printed when you bounce. 0 % = measure only.");
    describe(outputKnob, "OUTPUT", "Output volume trim in dB, applied before the loudness meter.");
    describe(learnButton, "LEARN MATCH", "Works out a gentle 10-band EQ (max 6 dB per band) that moves your mix's average toward the target, and switches it on. Play at least 10 s first.");
    describe(clearButton, "CLEAR", "Removes the learned Match EQ.");
    describe(hearAt, "HEAR AT", "Lets you hear your mix as it would sound at Phone, Quiet or Loud volume without touching your speakers. Listening only - set to OFF before bouncing.");

    for (juce::Component* c : std::initializer_list<juce::Component*> { &helpButton, &loadButton, &resetButton, &targetBox, &graph, &live, &grid, &loudness, &notes,
                                                                         &learnButton, &clearButton, &xrayButton, &loudBox, &listenKnob, &matchKnob, &outputKnob, &hearAt })
        addAndMakeVisible(c);

    setResizable(true, true);
    targetCaption.setText("TARGET", juce::dontSendNotification);
    targetCaption.setFont(font(Weight::Bold, 9.5f).withExtraKerningFactor(0.16f));
    targetCaption.setColour(juce::Label::textColourId, pal().inkMuted);
    targetCaption.setJustificationType(juce::Justification::centredRight);
    targetCaption.setInterceptsMouseClicks(false, false);
    addAndMakeVisible(targetCaption);
    addChildComponent(help);
    setResizeLimits(960, 700, 1600, 1100);
    setSize(1080, 780);
    timerCallback();
    startTimerHz(24);
}

EpsilonPhonEditor::~EpsilonPhonEditor() { setLookAndFeel(nullptr); }

juce::String EpsilonPhonEditor::statusText() const
{
    if (hoverHelp.isNotEmpty()) return hoverHelp;
    juce::String s = juce::String(proc.getSampleRateSafe() / 1000.0, 1) + " kHz  \xc2\xb7  zero latency  \xc2\xb7  hover anything for help";
    const int h = (int) proc.state.getRawParameterValue("hearAt")->load();
    if (h > 0) s << "  \xc2\xb7  HEAR AT " << EpsilonPhonProcessor::hearAtChoices()[h].toUpperCase() << " ON: MONITOR ONLY, TURN OFF BEFORE BOUNCING";
    return s;
}

void EpsilonPhonEditor::paint(juce::Graphics& g)
{
    if (! chrome.isValid() || chrome.getWidth() != getWidth() || chrome.getHeight() != getHeight())
    {
        chrome = juce::Image(juce::Image::RGB, juce::jmax(1, getWidth()), juce::jmax(1, getHeight()), true, juce::SoftwareImageType());
        juce::Graphics cg(chrome);
        cg.fillAll(pal().bg);
        drawPanel(cg, heroPanel.toFloat(), pal(), "Tonal balance vs target");
        drawPanel(cg, livePanel.toFloat(), pal());
        drawPanel(cg, loudPanel.toFloat(), pal(), "Loudness");
        drawPanel(cg, gridPanel.toFloat(), pal(), "Translation");
        drawPanel(cg, notesPanel.toFloat(), pal(), "Engineer notes");
        drawPanel(cg, ctrlPanel.toFloat(), pal(), "Monitor & match");
        cg.setColour(pal().inkMuted);
        cg.setFont(font(Weight::Bold, 9.5f).withExtraKerningFactor(0.16f));
        cg.drawText("HEAR AT", hearLabel, juce::Justification::centredLeft, false);
        cg.drawText("GOAL", goalLabel, juce::Justification::centredRight, false);
        // legend in hero panel
        auto lg = juce::Rectangle<int>(heroPanel.getX() + 300, heroPanel.getY() + 12, 360, 16).toFloat();
        const auto item = [&](juce::Colour c, const juce::String& t, float w)
        {
            cg.setColour(c); cg.fillRoundedRectangle(lg.getX(), lg.getCentreY() - 1.25f, 14.0f, 2.5f, 1.0f);
            cg.setColour(pal().inkMuted); cg.setFont(caption());
            cg.drawText(t, lg.withTrimmedLeft(19.0f), juce::Justification::centredLeft, false);
            lg.removeFromLeft(w);
        };
        item(pal().trim, "Target", 74); item(pal().ink, "Your mix (average)", 136); item(pal().live, "Live", 60);
    }
    g.drawImageAt(chrome, 0, 0);
    if (dragOver)
    {
        g.setColour(pal().live.withAlpha(0.12f));
        g.fillRect(getLocalBounds());
        g.setColour(pal().live);
        g.drawRect(getLocalBounds(), 2);
        g.setFont(font(Weight::Bold, 18.0f));
        g.drawText("Drop hit records to set the target", getLocalBounds(), juce::Justification::centred, false);
    }
}

void EpsilonPhonEditor::resized()
{
    chrome = {};
    auto r = getLocalBounds();
    header.setBounds(r.removeFromTop(size::header));
    footer.setBounds(r.removeFromBottom(size::footer));
    auto hc = header.getControlArea();
    resetButton.setBounds(hc.removeFromRight(80).withSizeKeepingCentre(80, 30));
    hc.removeFromRight(8);
    loadButton.setBounds(hc.removeFromRight(104).withSizeKeepingCentre(104, 30));
    hc.removeFromRight(8);
    targetBox.setBounds(hc.removeFromRight(180).withSizeKeepingCentre(180, 30));
    targetCaption.setBounds(hc.removeFromRight(62).withSizeKeepingCentre(62, 30));
    hc.removeFromRight(4);
    helpButton.setBounds(hc.removeFromRight(70).withSizeKeepingCentre(70, 30));
    help.setBounds(getLocalBounds().withTrimmedTop(size::header).withTrimmedBottom(size::footer));

    r = r.reduced(size::space4);
    const int bottomH = juce::jmax(220, (int) (r.getHeight() * 0.33f));
    auto bottom = r.removeFromBottom(bottomH);
    r.removeFromBottom(size::space3);
    loudPanel = r.removeFromRight(juce::jmax(300, (int) (r.getWidth() * 0.31f)));
    r.removeFromRight(size::space3);
    livePanel = r.removeFromBottom(112);
    r.removeFromBottom(size::space2);
    heroPanel = r;
    live.setBounds(livePanel.reduced(10, 8));
    const int w3 = (bottom.getWidth() - 2 * size::space3) / 3;
    gridPanel = bottom.removeFromLeft(w3 + 20); bottom.removeFromLeft(size::space3);
    ctrlPanel = bottom.removeFromRight(w3 - 20); bottom.removeFromRight(size::space3);
    notesPanel = bottom;

    xrayButton.setBounds(heroPanel.getRight() - 112, heroPanel.getY() + 8, 100, 26);
    graph.setBounds(heroPanel.reduced(10).withTrimmedTop(30));
    loudBox.setBounds(loudPanel.getRight() - 150, loudPanel.getY() + 8, 138, 26);
    goalLabel = { loudBox.getX() - 60, loudBox.getY(), 52, 26 };
    loudness.setBounds(loudPanel.reduced(16).withTrimmedTop(26));
    grid.setBounds(gridPanel.reduced(12).withTrimmedTop(24));
    notes.setBounds(notesPanel.reduced(16).withTrimmedTop(26));

    auto c = ctrlPanel.reduced(12).withTrimmedTop(24);
    auto hearRow = c.removeFromBottom(30);
    hearLabel = hearRow.removeFromLeft(64);
    hearAt.setBounds(hearRow);
    c.removeFromBottom(6);
    listenKnob.setBounds(c.removeFromLeft(124).withSizeKeepingCentre(124, juce::jmin(c.getHeight(), 144)));
    auto right = c.reduced(4, 0);
    auto btns = right.removeFromBottom(28);
    learnButton.setBounds(btns.removeFromLeft(btns.getWidth() * 3 / 5).reduced(2, 0));
    clearButton.setBounds(btns.reduced(2, 0));
    right.removeFromBottom(4);
    const int kw = right.getWidth() / 2;
    matchKnob.setBounds(right.removeFromLeft(kw));
    outputKnob.setBounds(right);
}

void EpsilonPhonEditor::chooseFiles()
{
    chooser = std::make_unique<juce::FileChooser>("Choose hit records (pick several to average them)",
                                                  juce::File::getSpecialLocation(juce::File::userMusicDirectory), "*.wav;*.aif;*.aiff;*.flac;*.mp3;*.m4a");
    chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles | juce::FileBrowserComponent::canSelectMultipleItems,
                         [this](const juce::FileChooser& fc)
                         {
                             const auto res = fc.getResults();
                             if (res.isEmpty()) return;
                             proc.loadReferences(res);
                             if (auto* t = proc.state.getParameter("target")) { t->beginChangeGesture(); t->setValueNotifyingHost(0.0f); t->endChangeGesture(); }
                         });
}

void EpsilonPhonEditor::filesDropped(const juce::StringArray& files, int, int)
{
    dragOver = false;
    juce::Array<juce::File> fs;
    for (auto& f : files) { juce::File file(f); if (file.existsAsFile()) fs.add(file); }
    proc.loadReferences(fs);
    if (auto* t = proc.state.getParameter("target")) { t->beginChangeGesture(); t->setValueNotifyingHost(0.0f); t->endChangeGesture(); }
    repaint();
}

void EpsilonPhonEditor::timerCallback()
{
    {
        // hover help: explain whatever is under the mouse in the bottom bar
        juce::String h;
        auto* under = juce::Desktop::getInstance().getMainMouseSource().getComponentUnderMouse();
        for (auto* c = under; c != nullptr && c != this && h.isEmpty(); c = c->getParentComponent())
            for (auto& e : helpEntries) if (e.comp == c) { h = e.name + ":  " + e.text; break; }
        if (under == nullptr || ! isParentOf(under)) h = {};
        if (h != hoverHelp) { hoverHelp = h; footer.repaint(); }
    }
    const auto snap = proc.getSnapshot();
    {
        // real-time meter: every tick, so bars, holds and names keep moving (and release in silence)
        const auto tgt = proc.currentTarget();
        const auto now = juce::Time::getMillisecondCounterHiRes();
        const float dt = lastLiveMs > 0 ? (float) juce::jlimit(0.005, 0.25, (now - lastLiveMs) * 0.001) : 0.04f;
        lastLiveMs = now;
        const bool fresh = snap.serial != lastLiveSerial;
        if (fresh) { lastLiveSerial = snap.serial; staleTicks = 0; } else ++staleTicks;
        live.update(staleTicks < 8 && snap.liveActive ? phon::liveDeviation(snap.live, tgt) : phon::LiveDev {}, dt, tgt.valid);
    }
    const int target = (int) proc.state.getRawParameterValue("target")->load();
    const float listen = proc.state.getRawParameterValue("listen")->load();
    const float match = proc.state.getRawParameterValue("match")->load();
    const int hear = (int) proc.state.getRawParameterValue("hearAt")->load();
    const int refSerial = proc.getReferenceSerial();
    const bool xray = xrayButton.getToggleState();
    const bool targetChanged = target != lastTarget || refSerial != lastRefSerial;
    const bool viewChanged = targetChanged || xray != lastXray || listen != lastListen || match != lastMatch || hear != lastHear;
    if (snap.serial == lastSerial && ! viewChanged) return;
    lastSerial = snap.serial; lastTarget = target; lastRefSerial = refSerial; lastXray = xray; lastListen = listen; lastMatch = match;
    if (hear != lastHear) { lastHear = hear; footer.repaint(); }

    PhonView v;
    v.snap = snap;
    v.target = proc.currentTarget();
    v.earView = xray;
    v.listen = listen;
    v.cmp = phon::compare(snap.average, v.target, listen);
    v.matchOn = proc.hasMatch() && match > 0.5f;
    if (v.matchOn)
        for (int i = 0; i < (int) v.matchResp.size(); ++i)
            v.matchResp[(size_t) i] = (float) proc.matchResponseDb(20.0 * std::pow(1000.0, i / (double) (v.matchResp.size() - 1)));
    graph.setView(v, targetChanged);
    grid.setView(v);

    const float lt = proc.loudnessTarget();
    juce::String tName = target == 0 ? juce::String("hits") : EpsilonPhonProcessor::targetChoices()[target];
    if ((int) proc.state.getRawParameterValue("loudTarget")->load() > 0) tName = {};
    loudness.set(snap.loud, lt, tName);

    // notes refresh at ~2 Hz so they're readable
    const auto now = juce::Time::getMillisecondCounter();
    if (viewChanged || now - lastNotes > 500)
    {
        lastNotes = now;
        std::vector<std::string> n;
        juce::String status;
        if (target == 0 && ! v.target.valid)
            n.push_back(proc.isLoadingReference() ? "Analysing your hit records..." : "Load or drop 1-10 hit records in your genre - they become the standard you're measured against");
        else if (snap.seconds < 8.0)
            n.push_back("Play your mix (the whole song is best). Notes appear after ~8 seconds of audio");
        else
            n = phon::suggestions(v.cmp, v.target, snap.loud, lt);
        status = (target == 0 ? proc.getReferenceMessage() : juce::String(v.target.name) + " (modelled curve)")
               + juce::String::fromUTF8("  \xc2\xb7  ") + juce::String((int) snap.seconds) + " s measured";
        notes.set(n, status);
    }
}
