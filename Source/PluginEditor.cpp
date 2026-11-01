#include "PluginEditor.h"

using namespace juce;

//==============================================================================
//  LabeledKnob
//==============================================================================
LabeledKnob::LabeledKnob (AudioProcessorValueTreeState& state, const String& paramID, const String& t)
    : title (t)
{
    slider.setSliderStyle (Slider::RotaryHorizontalVerticalDrag);
    slider.setTextBoxStyle (Slider::TextBoxBelow, false, 84, 18);
    slider.setColour (Slider::textBoxTextColourId, Silver::navy);
    slider.setPopupDisplayEnabled (false, false, nullptr);
    addAndMakeVisible (slider);
    attachment = std::make_unique<AudioProcessorValueTreeState::SliderAttachment> (state, paramID, slider);
    if (auto* p = state.getParameter (paramID))
        slider.setDoubleClickReturnValue (true, p->convertFrom0to1 (p->getDefaultValue()));
}

void LabeledKnob::resized()
{
    slider.setBounds (getLocalBounds().withTrimmedTop (16));
}

void LabeledKnob::paint (Graphics& g)
{
    g.setColour (isEnabled() ? Silver::navy : Silver::steel.withAlpha (0.6f));
    g.setFont (FontOptions (12.5f, Font::bold));
    g.drawText (title.toUpperCase(), getLocalBounds().removeFromTop (16), Justification::centred);
}

//==============================================================================
//  LevelerView
//==============================================================================
LevelerView::LevelerView (VXProcessor& p) : proc (p)
{
    setMouseCursor (MouseCursor::UpDownResizeCursor);
    startTimerHz (30);
}

LevelerView::~LevelerView() { stopTimer(); }

std::vector<LevelerView::Lane> LevelerView::getLanes() const
{
    std::vector<Lane> lanes;
    auto* renderer = proc.getRenderer();
    if (renderer == nullptr) return lanes;

    for (auto* region : renderer->getPlaybackRegions())
    {
        Lane l;
        l.region = region;
        l.mod = dynamic_cast<VXAudioModification*> (region->getAudioModification());
        if (l.mod == nullptr) continue;
        l.source = dynamic_cast<VXAudioSource*> (l.mod->getAudioSource());
        if (l.source == nullptr) continue;
        l.playStart = region->getStartInPlaybackTime();
        l.playEnd   = region->getEndInPlaybackTime();
        l.modStart  = region->getStartInAudioModificationTime();
        l.name = convertOptionalARAString (region->getEffectiveName());
        if (l.name.isEmpty()) l.name = l.mod->getDisplayName();
        lanes.push_back (l);
    }
    std::sort (lanes.begin(), lanes.end(), [] (const Lane& a, const Lane& b) { return a.playStart < b.playStart; });
    return lanes;
}

std::shared_ptr<const vx::GainCurve> LevelerView::getFocusCurve() const
{
    const auto lanes = getLanes();
    const double t = proc.playheadSeconds.load();
    for (auto& l : lanes)
        if (t >= l.playStart && t < l.playEnd)
            if (auto c = proc.getCurveFor (l.mod)) return c;
    for (auto& l : lanes)
        if (auto c = proc.getCurveFor (l.mod)) return c;
    return nullptr;
}

Rectangle<int> LevelerView::getPlotArea() const
{
    return getLocalBounds().reduced (10, 8).withTrimmedRight (42).withTrimmedBottom (18);
}

float LevelerView::xForTime (double t) const
{
    const auto a = getPlotArea().toFloat();
    return a.getX() + (float) ((t - viewStart) / jmax (1.0e-6, viewEnd - viewStart)) * a.getWidth();
}

double LevelerView::timeForX (float x) const
{
    const auto a = getPlotArea().toFloat();
    return viewStart + (double) ((x - a.getX()) / jmax (1.0f, a.getWidth())) * (viewEnd - viewStart);
}

float LevelerView::gainScaleDb() const
{
    return jmax (12.0f, std::ceil (proc.getSettings().rangeDb / 6.0f) * 6.0f);
}

float LevelerView::yForGain (float db) const
{
    const auto a = getPlotArea().toFloat();
    const float s = gainScaleDb();
    return a.getCentreY() - jlimit (-1.0f, 1.0f, db / s) * a.getHeight() * 0.46f;
}

void LevelerView::ensureView (const std::vector<DrawLane>& lanes)
{
    if (! autoFit || lanes.empty()) return;
    double a = lanes.front().playStart, b = lanes.front().playEnd;
    for (auto& l : lanes) { a = jmin (a, l.playStart); b = jmax (b, l.playEnd); }
    const double pad = jmax (0.05, (b - a) * 0.01);
    viewStart = a - pad;
    viewEnd = b + pad;
}

void LevelerView::timerCallback()
{
    bool need = proc.playing.load() || ! proc.isBoundToARA();

    if (proc.getCurvesVersion() != lastCurvesVersion) { lastCurvesVersion = proc.getCurvesVersion(); need = true; }

    for (auto& l : getLanes())
        if (l.source->getState() == VXAudioSource::analysing) { need = true; break; }

    if (need) repaint();
}

Colour LevelerView::regionColour (int type)
{
    switch (type)
    {
        case 0:  return Colour (0xff6f7c8f);   // pausa (gate)
        case 1:  return Colour (0xff2f8f8a);   // respiro
        default: return Colour (0xff7a5ba6);   // sibilante
    }
}

void LevelerView::paintGrid (Graphics& g, Rectangle<int> area, float scaleDb)
{
    const auto a = area.toFloat();
    g.setFont (FontOptions (11.0f));
    for (float db = -scaleDb; db <= scaleDb + 0.01f; db += 6.0f)
    {
        const float y = yForGain (db);
        g.setColour (std::abs (db) < 0.01f ? Silver::steel.withAlpha (0.55f) : Silver::grid);
        g.drawHorizontalLine (roundToInt (y), a.getX(), a.getRight());
        g.setColour (Silver::steel);
        g.drawText ((db > 0 ? "+" : "") + String ((int) db), (int) a.getRight() + 4, (int) y - 7, 36, 14, Justification::centredLeft);
    }
}

void LevelerView::paint (Graphics& g)
{
    auto r = getLocalBounds().toFloat();
    g.setColour (Colours::white.withAlpha (0.55f));
    g.fillRoundedRectangle (r, 8.0f);
    g.setColour (Silver::border);
    g.drawRoundedRectangle (r.reduced (0.5f), 8.0f, 1.0f);

    const auto plot = getPlotArea();
    if (proc.isBoundToARA() || ! previewLanes.empty())
        paintARA (g, plot);
    else
        paintRealtime (g, plot);
}

void LevelerView::paintARA (Graphics& g, Rectangle<int> plot)
{
    std::vector<DrawLane> lanes (previewLanes);
    if (lanes.empty())
    {
        for (auto& l : getLanes())
        {
            DrawLane d;
            d.analysis = l.source->getAnalysis();
            d.curve = proc.getCurveFor (l.mod);
            d.playStart = l.playStart; d.playEnd = l.playEnd; d.modStart = l.modStart;
            d.name = l.name;
            d.state = (int) l.source->getState();
            d.progress = l.source->getProgress();
            lanes.push_back (d);
        }
    }
    if (lanes.empty())
    {
        g.setColour (Silver::navy);
        g.setFont (FontOptions (16.0f, Font::bold));
        g.drawFittedText ("Modo ARA ativo - aguardando clipes", plot.withTrimmedBottom (plot.getHeight() / 2), Justification::centredBottom, 1);
        g.setColour (Silver::steel);
        g.setFont (FontOptions (13.0f));
        g.drawFittedText ("No REAPER, insira o VX_Leveler SILVER como FX do item (take) ou da trilha.\n"
                          "A analise do take inteiro comeca automaticamente.",
                          plot.withTrimmedTop (plot.getHeight() / 2 + 8), Justification::centredTop, 3);
        return;
    }

    ensureView (lanes);
    const float scaleDb = gainScaleDb();
    paintGrid (g, plot, scaleDb);

    const auto pf = plot.toFloat();
    const float mid = pf.getCentreY();
    const float half = pf.getHeight() * 0.46f;

    Graphics::ScopedSaveState save (g);
    g.reduceClipRegion (plot);

    for (auto& lane : lanes)
    {
        const float x0 = xForTime (lane.playStart), x1 = xForTime (lane.playEnd);
        if (x1 < pf.getX() || x0 > pf.getRight()) continue;

        auto box = Rectangle<float> (x0, pf.getY(), x1 - x0, pf.getHeight());
        g.setColour (Silver::panel.withAlpha (0.7f));
        g.fillRoundedRectangle (box, 4.0f);
        g.setColour (Silver::border);
        g.drawRoundedRectangle (box.reduced (0.5f), 4.0f, 1.0f);

        auto analysis = lane.analysis;
        auto curve = lane.curve;

        if (analysis == nullptr)
        {
            g.setColour (Silver::navyMid);
            g.setFont (FontOptions (13.0f, Font::bold));
            const auto st = lane.state;
            String msg = st == VXAudioSource::analysing ? "Analisando... " + String (roundToInt (lane.progress * 100.0f)) + " %"
                       : st == VXAudioSource::failed    ? "Falha na leitura do audio (tentando de novo)"
                                                        : "Aguardando acesso as amostras...";
            g.drawFittedText (msg, box.toNearestInt(), Justification::centred, 1);
            continue;
        }

        const double fs = analysis->sampleRate;
        const int hop = analysis->hopSamples;
        const int nF = analysis->numFrames();

        auto toFrame = [&] (double playT)
        {
            const double srcT = playT - lane.playStart + lane.modStart;
            return (int) std::floor (srcT * fs / hop);
        };

        // escala linear normalizada pelo maior pico (original ou processado) do clipe
        float maxPk = 1.0e-4f;
        for (int f = 0; f < nF; ++f)
        {
            const float pk = analysis->peak[(size_t) f];
            maxPk = jmax (maxPk, pk);
            if (curve != nullptr && f < curve->numFrames()) maxPk = jmax (maxPk, pk * curve->gainLin[(size_t) f]);
        }
        auto ampToY = [maxPk] (float a) { return jlimit (0.0f, 1.0f, a / (maxPk * 1.05f)); };

        Path gainPath, autoPath;
        bool started = false;
        const int xa = (int) jmax (pf.getX(), x0), xb = (int) jmin (pf.getRight(), x1);

        RectangleList<float> origBars, procBars, editBars;

        for (int x = xa; x < xb; ++x)
        {
            int f0 = toFrame (timeForX ((float) x));
            int f1 = jmax (f0 + 1, toFrame (timeForX ((float) x + 1.0f)));
            f0 = jlimit (0, nF, f0); f1 = jlimit (0, nF, f1);
            if (f1 <= f0) continue;

            float po = 0.0f, pp = 0.0f, gsum = 0.0f, asum = 0.0f;
            for (int f = f0; f < f1; ++f)
            {
                const float pk = analysis->peak[(size_t) f];
                po = jmax (po, pk);
                if (curve != nullptr && f < curve->numFrames())
                {
                    pp = jmax (pp, pk * curve->gainLin[(size_t) f]);
                    gsum += curve->gainDb[(size_t) f];
                    asum += curve->autoDb[(size_t) f];
                }
                else pp = po;
            }
            const float n = (float) (f1 - f0);
            const float hO = ampToY (po) * half, hP = ampToY (pp) * half;
            origBars.addWithoutMerging ({ (float) x, mid - hO, 1.0f, 2.0f * hO });
            procBars.addWithoutMerging ({ (float) x, mid - hP, 1.0f, 2.0f * hP });

            if (curve != nullptr)
            {
                const float gy = yForGain (gsum / n), ay = yForGain (asum / n);
                if (! started) { gainPath.startNewSubPath ((float) x, gy); autoPath.startNewSubPath ((float) x, ay); started = true; }
                else           { gainPath.lineTo ((float) x, gy);          autoPath.lineTo ((float) x, ay); }
                if (std::abs (gy - ay) > 0.5f)
                    editBars.addWithoutMerging ({ (float) x, jmin (gy, ay), 1.0f, std::abs (gy - ay) });
            }
        }

        g.setColour (Silver::waveOrig);
        g.fillRectList (origBars);
        g.setColour (Silver::waveProc.withAlpha (0.9f));
        g.fillRectList (procBars);

        // regioes detectadas (pausas / respiros / sibilantes)
        if (curve != nullptr)
        {
            const auto strip = Rectangle<float> (pf.getX(), pf.getBottom() - 15.0f, pf.getWidth(), 14.0f);
            for (auto& r : curve->regions)
            {
                const double t0 = r.startFrame * (double) hop / fs - lane.modStart + lane.playStart;
                const double t1 = r.endFrame   * (double) hop / fs - lane.modStart + lane.playStart;
                const float rx0 = jmax (x0, xForTime (t0)), rx1 = jmin (x1, xForTime (t1));
                if (rx1 < pf.getX() || rx0 > pf.getRight() || rx1 - rx0 < 0.5f) continue;

                const auto col = regionColour ((int) r.type);
                const auto band = Rectangle<float> (rx0, pf.getY(), jmax (1.0f, rx1 - rx0), pf.getHeight() - 16.0f);
                const auto tag  = Rectangle<float> (rx0, strip.getY(), jmax (2.0f, rx1 - rx0), strip.getHeight());
                if (r.enabled)
                {
                    g.setColour (col.withAlpha (0.10f));
                    g.fillRect (band);
                    g.setColour (col.withAlpha (0.85f));
                    g.fillRoundedRectangle (tag, 3.0f);
                }
                else
                {
                    g.setColour (col.withAlpha (0.7f));
                    g.drawRoundedRectangle (tag.reduced (0.5f), 3.0f, 1.0f);
                }
                if (tag.getWidth() > 34.0f)
                {
                    g.setColour (r.enabled ? Colours::white : col);
                    g.setFont (FontOptions (10.0f, Font::bold));
                    const String txt = r.type == vx::RegionType::gate ? "PAUSA" : r.type == vx::RegionType::breath ? "RESP" : "S";
                    g.drawText (txt + " " + (r.depthDb <= vx::kInfDb + 0.1f ? String ("-inf") : String (roundToInt (r.depthDb))),
                                tag.toNearestInt(), Justification::centred, false);
                }
            }
        }

        if (started)
        {
            g.setColour (Silver::contrast.withAlpha (0.45f));
            g.fillRectList (editBars);

            Path dashed;
            const float dashes[] = { 4.0f, 3.0f };
            PathStrokeType (1.0f).createDashedStroke (dashed, autoPath, dashes, 2);
            g.setColour (Silver::steel.withAlpha (0.8f));
            g.fillPath (dashed);

            g.setColour (Colours::white.withAlpha (0.9f));
            g.strokePath (gainPath, PathStrokeType (5.0f, PathStrokeType::curved, PathStrokeType::rounded));
            g.setColour (Silver::navy);
            g.strokePath (gainPath, PathStrokeType (2.2f, PathStrokeType::curved, PathStrokeType::rounded));
        }

        // nome do clipe
        g.setColour (Silver::navy);
        g.setFont (FontOptions (12.0f, Font::bold));
        g.drawText (lane.name, box.reduced (6.0f, 4.0f).toNearestInt(), Justification::topLeft, true);
    }

    // legenda das regioes
    {
        const char* names[3] = { "pausa", "respiro", "sibilante" };
        float lx = pf.getRight() - 230.0f;
        const float ly = pf.getY() + 6.0f;
        g.setFont (FontOptions (11.0f, Font::bold));
        for (int t = 0; t < 3; ++t)
        {
            g.setColour (regionColour (t).withAlpha (0.85f));
            g.fillRoundedRectangle (lx, ly + 2.0f, 10.0f, 10.0f, 2.0f);
            g.setColour (Silver::steel);
            g.drawText (names[t], (int) lx + 14, (int) ly, 70, 14, Justification::centredLeft);
            lx += t == 0 ? 60.0f : 70.0f;
        }
    }

    // nos (ferramenta Nos)
    {
        const auto drawLanes = getLanes();
        for (auto& l : drawLanes)
        {
            const auto& nodes = l.mod->nodes;
            if (nodes.empty()) continue;
            Path env;
            bool first = true;
            for (int i = 0; i < (int) nodes.size(); ++i)
            {
                const double tPlay = nodes[(size_t) i].t - l.modStart + l.playStart;
                const Point<float> pt (xForTime (tPlay), nodeY (l, nodes[(size_t) i].t));
                if (first) { env.startNewSubPath (pt); first = false; } else env.lineTo (pt);
            }
            if (tool == Tool::nodes)
            {
                g.setColour (Silver::contrast.withAlpha (0.35f));
                g.strokePath (env, PathStrokeType (1.0f));
            }
            for (int i = 0; i < (int) nodes.size(); ++i)
            {
                const double tPlay = nodes[(size_t) i].t - l.modStart + l.playStart;
                const float nx = xForTime (tPlay), ny = nodeY (l, nodes[(size_t) i].t);
                const bool hot = (hoverNode.mod == l.mod && hoverNode.index == i) || (dragNode.mod == l.mod && dragNode.index == i);
                const float r = tool == Tool::nodes ? (hot ? 6.5f : 5.0f) : 3.0f;
                g.setColour (Colours::white);
                g.fillEllipse (nx - r - 1.5f, ny - r - 1.5f, 2 * r + 3.0f, 2 * r + 3.0f);
                g.setColour (hot ? Silver::contrast : Silver::navy);
                g.fillEllipse (nx - r, ny - r, 2 * r, 2 * r);
                if (hot)
                {
                    const float v = nodes[(size_t) i].db;
                    g.setFont (FontOptions (11.0f, Font::bold));
                    g.setColour (Silver::navy);
                    g.drawText ((v > 0 ? "+" : "") + String (v, 1) + " dB", (int) nx - 30, (int) ny - 24, 60, 14, Justification::centred);
                }
            }
        }
    }

    // pincel de edicao
    if (tool == Tool::brush && (hovering || dragging))
    {
        const double centre = dragging ? dragCentre : timeForX (hoverX);
        const float bx0 = xForTime (centre - brushSeconds * 0.5), bx1 = xForTime (centre + brushSeconds * 0.5);
        g.setColour (Silver::contrast.withAlpha (0.10f));
        g.fillRect (Rectangle<float> (bx0, pf.getY(), bx1 - bx0, pf.getHeight()));
        const float dashes[] = { 5.0f, 4.0f };
        g.setColour (Silver::contrast.withAlpha (0.9f));
        g.drawDashedLine ({ bx0, pf.getY(), bx0, pf.getBottom() }, dashes, 2, 1.2f);
        g.drawDashedLine ({ bx1, pf.getY(), bx1, pf.getBottom() }, dashes, 2, 1.2f);
    }

    // cursor de reproducao
    const float px = xForTime (proc.playheadSeconds.load());
    if (px >= pf.getX() && px <= pf.getRight())
    {
        g.setColour (Silver::navy);
        g.drawLine (px, pf.getY(), px, pf.getBottom(), 1.5f);
    }

    // regua de tempo
    g.setColour (Silver::steel);
    g.setFont (FontOptions (11.0f));
    const double span = viewEnd - viewStart;
    const double steps[] = { 0.1, 0.25, 0.5, 1, 2, 5, 10, 15, 30, 60, 120, 300, 600 };
    double step = 600;
    for (auto s : steps) if (span / s <= 12.0) { step = s; break; }
    g.reduceClipRegion (getLocalBounds());
    for (double t = std::ceil (viewStart / step) * step; t <= viewEnd; t += step)
    {
        const float x = xForTime (t);
        const int m = (int) (t / 60.0);
        const double sec = t - m * 60.0;
        const String txt = String (m) + ":" + (sec < 10 ? "0" : "") + String (sec, step < 1.0 ? 2 : 0);
        g.drawText (txt, (int) x - 30, plot.getBottom() + 2, 60, 14, Justification::centred);
    }
}

void LevelerView::paintRealtime (Graphics& g, Rectangle<int> plot)
{
    const float scaleDb = gainScaleDb();
    paintGrid (g, plot, scaleDb);
    const auto pf = plot.toFloat();
    auto& rt = proc.getRealtime();
    const int N = vx::RealtimeLeveler::historySize;
    const int w = rt.historyWrite;

    Path inPath, gainPath;
    for (int i = 0; i < N; ++i)
    {
        const int idx = (w + i) % N;
        const float x = pf.getX() + pf.getWidth() * (float) i / (float) (N - 1);
        const float inDb = jlimit (-60.0f, 0.0f, rt.historyIn[idx]);
        const float yi = pf.getBottom() - (inDb + 60.0f) / 60.0f * pf.getHeight();
        const float yg = yForGain (rt.history[idx]);
        if (i == 0) { inPath.startNewSubPath (x, pf.getBottom()); inPath.lineTo (x, yi); gainPath.startNewSubPath (x, yg); }
        else        { inPath.lineTo (x, yi); gainPath.lineTo (x, yg); }
    }
    inPath.lineTo (pf.getRight(), pf.getBottom());
    inPath.closeSubPath();
    g.setColour (Silver::waveOrig.withAlpha (0.55f));
    g.fillPath (inPath);
    g.setColour (Silver::accent);
    g.strokePath (gainPath, PathStrokeType (2.0f));

    g.setColour (Silver::navy);
    g.setFont (FontOptions (13.0f, Font::bold));
    g.drawText ("Modo tempo real (host sem ARA) - lookahead 300 ms", plot.reduced (8, 6), Justification::topLeft);
    g.setColour (Silver::steel);
    g.setFont (FontOptions (11.0f));
    g.drawText ("ultimos 10 s  |  cinza: nivel de entrada  |  azul: ganho aplicado", plot.reduced (8, 6), Justification::bottomLeft);
}

//------------------------------------------------------------------------------
void LevelerView::mouseMove (const MouseEvent& e)
{
    hovering = proc.isBoundToARA() && getPlotArea().contains (e.getPosition());
    hoverX = (float) e.x;
    if (tool == Tool::nodes)
    {
        hoverNode = findNode (e.position);
        setMouseCursor (hoverNode.mod != nullptr ? MouseCursor::DraggingHandCursor : MouseCursor::CrosshairCursor);
    }
    repaint();
}

void LevelerView::mouseExit (const MouseEvent&)
{
    hovering = false;
    repaint();
}

void LevelerView::mouseDown (const MouseEvent& e)
{
    if (! proc.isBoundToARA() || ! getPlotArea().contains (e.getPosition()))
        return;
    if (e.mods.isPopupMenu() && tool != Tool::nodes)
        return;

    // clique na faixa de regioes: liga/desliga a regiao
    if ((float) e.y >= (float) getPlotArea().getBottom() - 16.0f)
    {
        const double t = timeForX ((float) e.x);
        for (auto& l : getLanes())
        {
            if (t < l.playStart || t >= l.playEnd) continue;
            auto curve = proc.getCurveFor (l.mod);
            auto analysis = l.source->getAnalysis();
            if (curve == nullptr || analysis == nullptr) continue;
            const double srcT = t - l.playStart + l.modStart;
            const int f = (int) (srcT * analysis->sampleRate / analysis->hopSamples);
            const vx::Region* hit = nullptr;
            for (auto& r : curve->regions)
                if (f >= r.startFrame - 2 && f < r.endFrame + 2 && (hit == nullptr || r.type == vx::RegionType::sibilance))
                    hit = &r;
            if (hit != nullptr)
            {
                l.mod->toggleRegion (hit->type, hit->startFrame);
                l.mod->notifyEdited();
                proc.refreshCurves();
                if (onEditsChanged) onEditsChanged();
                repaint();
            }
            return;
        }
        return;
    }

    autoFit = false;

    if (tool == Tool::nodes)
    {
        mouseDownNodes (e);
        return;
    }

    dragging = true;
    dragCentre = timeForX ((float) e.x);
    dragStartY = (float) e.y;

    for (auto& l : getLanes())
        if (l.source->getAnalysis() != nullptr)
            l.mod->ensureSize (l.source->getAnalysis()->numFrames());
    pushUndo();
    dragSnapshot = undoStack.back();
}

void LevelerView::pushUndo (VXAudioModification* onlyThis)
{
    std::vector<Snapshot> snap;
    for (auto& l : getLanes())
    {
        if (onlyThis != nullptr && l.mod != onlyThis) continue;
        bool dup = false;
        for (auto& sn : snap) if (sn.mod == l.mod) dup = true;
        if (! dup) snap.push_back ({ l.mod, l.mod->editsDb, l.mod->nodes });
    }
    undoStack.push_back (std::move (snap));
    if (undoStack.size() > 50) undoStack.erase (undoStack.begin());
}

void LevelerView::setTool (Tool t)
{
    tool = t;
    hoverNode = {};
    setMouseCursor (t == Tool::brush ? MouseCursor::UpDownResizeCursor : MouseCursor::CrosshairCursor);
    repaint();
}

float LevelerView::nodeY (const Lane& lane, double srcT) const
{
    if (auto c = proc.getCurveFor (lane.mod))
        return yForGain (c->gainDbAtTime (srcT));
    return yForGain (lane.mod->nodeEnvAt (srcT));
}

LevelerView::NodeHit LevelerView::findNode (Point<float> pos) const
{
    NodeHit best;
    float bestD = 10.0f;
    for (auto& l : getLanes())
    {
        for (int i = 0; i < (int) l.mod->nodes.size(); ++i)
        {
            const auto& nd = l.mod->nodes[(size_t) i];
            const double tPlay = nd.t - l.modStart + l.playStart;
            if (tPlay < l.playStart - 1.0e-6 || tPlay > l.playEnd + 1.0e-6) continue;
            const float d = pos.getDistanceFrom ({ xForTime (tPlay), nodeY (l, nd.t) });
            if (d < bestD)
            {
                bestD = d;
                best.mod = l.mod; best.index = i;
                best.playStart = l.playStart; best.modStart = l.modStart;
                best.modEnd = l.modStart + (l.playEnd - l.playStart);
            }
        }
    }
    return best;
}

void LevelerView::mouseDownNodes (const MouseEvent& e)
{
    auto hit = findNode (e.position);

    // Alt+clique ou botao direito: apaga o no
    if (hit.mod != nullptr && (e.mods.isAltDown() || e.mods.isPopupMenu()))
    {
        pushUndo (hit.mod);
        hit.mod->nodes.erase (hit.mod->nodes.begin() + hit.index);
        hit.mod->notifyEdited();
        hoverNode = {};
        proc.refreshCurves();
        if (onEditsChanged) onEditsChanged();
        repaint();
        return;
    }
    if (e.mods.isPopupMenu()) return;

    if (hit.mod == nullptr)
    {
        // clique fora de um no: cria um no novo no valor atual do envelope (o som nao muda)
        const double t = timeForX ((float) e.x);
        for (auto& l : getLanes())
        {
            if (t < l.playStart || t >= l.playEnd) continue;
            const double srcT = t - l.playStart + l.modStart;
            pushUndo (l.mod);
            auto& nodes = l.mod->nodes;
            VXAudioModification::Node nd { srcT, l.mod->nodeEnvAt (srcT) };
            auto it = std::lower_bound (nodes.begin(), nodes.end(), srcT,
                                        [] (const VXAudioModification::Node& a, double v) { return a.t < v; });
            const int idx = (int) std::distance (nodes.begin(), it);
            nodes.insert (it, nd);
            ++l.mod->editsVersion;
            hit.mod = l.mod; hit.index = idx;
            hit.playStart = l.playStart; hit.modStart = l.modStart;
            hit.modEnd = l.modStart + (l.playEnd - l.playStart);
            break;
        }
        if (hit.mod == nullptr) return;
    }
    else
    {
        pushUndo (hit.mod);
    }

    dragNode = hit;
    dragNodeStartT  = hit.mod->nodes[(size_t) hit.index].t;
    dragNodeStartDb = hit.mod->nodes[(size_t) hit.index].db;
    dragStartX = (float) e.x;
    dragStartY = (float) e.y;
    dragging = true;
    proc.refreshCurves();
    repaint();
}

void LevelerView::mouseDragNodes (const MouseEvent& e)
{
    auto* mod = dragNode.mod;
    if (mod == nullptr || dragNode.index < 0 || dragNode.index >= (int) mod->nodes.size()) return;

    const float pxPerDb = getPlotArea().getHeight() * 0.46f / gainScaleDb();
    const float fine = e.mods.isShiftDown() ? 0.2f : 1.0f;
    auto& nd = mod->nodes[(size_t) dragNode.index];

    nd.db = jlimit (-24.0f, 24.0f, dragNodeStartDb + (dragStartY - (float) e.y) / pxPerDb * fine);

    if (! e.mods.isShiftDown())   // Shift: so vertical
    {
        const double secPerPx = (viewEnd - viewStart) / jmax (1, getPlotArea().getWidth());
        double t = dragNodeStartT + ((float) e.x - dragStartX) * secPerPx;
        const double lo = dragNode.index > 0 ? mod->nodes[(size_t) dragNode.index - 1].t + 0.01 : dragNode.modStart;
        const double hi = dragNode.index + 1 < (int) mod->nodes.size() ? mod->nodes[(size_t) dragNode.index + 1].t - 0.01 : dragNode.modEnd;
        nd.t = jlimit (lo, jmax (lo, hi), t);
    }

    ++mod->editsVersion;
    proc.refreshCurves();
    repaint();
}

void LevelerView::mouseDrag (const MouseEvent& e)
{
    if (! dragging) return;
    if (tool == Tool::nodes) { mouseDragNodes (e); return; }

    const float pxPerDb = getPlotArea().getHeight() * 0.46f / gainScaleDb();
    const float fine = e.mods.isShiftDown() ? 0.2f : 1.0f;
    const float delta = (dragStartY - (float) e.y) / pxPerDb * fine;
    const double halfW = brushSeconds * 0.5;

    for (auto& l : getLanes())
    {
        auto analysis = l.source->getAnalysis();
        if (analysis == nullptr) continue;
        const Snapshot* snap = nullptr;
        for (auto& s : dragSnapshot) if (s.mod == l.mod) snap = &s;
        if (snap == nullptr) continue;

        const double a = jmax (l.playStart, dragCentre - halfW), b = jmin (l.playEnd, dragCentre + halfW);
        if (b <= a) continue;

        const double fs = analysis->sampleRate;
        const int hop = analysis->hopSamples;
        auto& edits = l.mod->editsDb;
        const int f0 = jmax (0, (int) std::floor ((a - l.playStart + l.modStart) * fs / hop));
        const int f1 = jmin ((int) edits.size(), (int) std::ceil ((b - l.playStart + l.modStart) * fs / hop));

        for (int f = f0; f < f1; ++f)
        {
            const double tPlay = ((f + 0.5) * hop / fs) - l.modStart + l.playStart;
            const double u = (tPlay - dragCentre) / halfW;
            if (std::abs (u) >= 1.0) { edits[(size_t) f] = snap->edits[(size_t) f]; continue; }
            const float w = (float) (0.5 * (1.0 + std::cos (MathConstants<double>::pi * u)));
            // topo achatado: 60% central com peso total
            const float wf = jlimit (0.0f, 1.0f, w * 1.6f);
            edits[(size_t) f] = jlimit (-24.0f, 24.0f, snap->edits[(size_t) f] + delta * wf);
        }
        ++l.mod->editsVersion;
    }

    proc.refreshCurves();
    repaint();
}

void LevelerView::mouseUp (const MouseEvent&)
{
    if (! dragging) return;
    dragging = false;
    if (dragNode.mod != nullptr) { dragNode.mod->notifyEdited(); dragNode = {}; }
    for (auto& sn : dragSnapshot) sn.mod->notifyEdited();
    dragSnapshot.clear();
    proc.refreshCurves();
    if (onEditsChanged) onEditsChanged();
    repaint();
}

void LevelerView::mouseDoubleClick (const MouseEvent& e)
{
    if (tool == Tool::nodes)
    {
        auto hit = findNode (e.position);
        if (hit.mod != nullptr)
        {
            // o 1o clique ja guardou o estado para desfazer
            hit.mod->nodes.erase (hit.mod->nodes.begin() + hit.index);
            hit.mod->notifyEdited();
            hoverNode = {};
            proc.refreshCurves();
            if (onEditsChanged) onEditsChanged();
            repaint();
            return;
        }
    }
    autoFit = true;
    repaint();
}

void LevelerView::mouseWheelMove (const MouseEvent& e, const MouseWheelDetails& w)
{
    if (! proc.isBoundToARA()) return;
    const float dy = std::abs (w.deltaY) > 0.0f ? w.deltaY : w.deltaX;

    if (e.mods.isShiftDown())
    {
        brushSeconds = jlimit (0.05, 20.0, brushSeconds * std::pow (2.0, -dy * 2.0));
    }
    else if (e.mods.isCommandDown() || e.mods.isCtrlDown())
    {
        autoFit = false;
        const double t = timeForX ((float) e.x);
        const double z = std::pow (2.0, -dy * 2.0);
        viewStart = t - (t - viewStart) * z;
        viewEnd   = t + (viewEnd - t) * z;
        if (viewEnd - viewStart < 0.2) { const double c = 0.5 * (viewStart + viewEnd); viewStart = c - 0.1; viewEnd = c + 0.1; }
    }
    else
    {
        autoFit = false;
        const double shift = -(double) dy * (viewEnd - viewStart) * 0.25;
        viewStart += shift; viewEnd += shift;
    }
    repaint();
}

void LevelerView::undo()
{
    if (undoStack.empty()) return;
    auto snap = std::move (undoStack.back());
    undoStack.pop_back();

    const auto lanes = getLanes();
    for (auto& s : snap)
    {
        bool alive = false;
        for (auto& l : lanes) if (l.mod == s.mod) alive = true;
        if (! alive) continue;
        s.mod->editsDb = s.edits;
        s.mod->nodes = s.nodes;
        s.mod->notifyEdited();
    }
    proc.refreshCurves();
    if (onEditsChanged) onEditsChanged();
    repaint();
}

void LevelerView::clearEdits()
{
    std::vector<Snapshot> snap;
    for (auto& l : getLanes())
    {
        bool dup = false;
        for (auto& s : snap) if (s.mod == l.mod) dup = true;
        if (dup || ! l.mod->hasEdits()) continue;
        snap.push_back ({ l.mod, l.mod->editsDb, l.mod->nodes });
        std::fill (l.mod->editsDb.begin(), l.mod->editsDb.end(), 0.0f);
        l.mod->nodes.clear();
        l.mod->notifyEdited();
    }
    if (! snap.empty()) undoStack.push_back (std::move (snap));
    proc.refreshCurves();
    if (onEditsChanged) onEditsChanged();
    repaint();
}

//==============================================================================
//  Exportacao WAV (thread com janela de progresso)
//==============================================================================
namespace
{
class WavExportJob : public ThreadWithProgressWindow
{
public:
    struct Item
    {
        std::unique_ptr<ARAAudioSourceReader> reader;
        std::shared_ptr<const vx::GainCurve> curve;
        File file;
    };

    WavExportJob (std::vector<Item>&& itemsIn, float gain)
        : ThreadWithProgressWindow ("VX_Leveler SILVER - exportando WAV", true, true),
          items (std::move (itemsIn)), outGain (gain) {}

    void run() override
    {
        int64 totalAll = 0, doneAll = 0;
        for (auto& it : items) totalAll += it.reader->lengthInSamples;

        for (auto& it : items)
        {
            if (threadShouldExit()) return;
            auto& rd = *it.reader;
            const int nch = jlimit (1, 2, (int) rd.numChannels);
            const int bits = 24;
            it.file.deleteFile();
            std::unique_ptr<OutputStream> os (it.file.createOutputStream());
            if (os == nullptr) { errors << it.file.getFullPathName() << "\n"; continue; }

            WavAudioFormat wav;
            std::unique_ptr<AudioFormatWriter> writer (wav.createWriterFor (os.get(), rd.sampleRate, (unsigned int) nch, bits, {}, 0));
            if (writer == nullptr) { errors << it.file.getFullPathName() << "\n"; continue; }
            os.release();

            const int chunk = 1 << 15;
            AudioBuffer<float> buf (nch, chunk);
            vx::SpectralDeEsser deEsser;
            deEsser.prepare (rd.sampleRate, nch);
            for (int64 pos = 0; pos < rd.lengthInSamples; pos += chunk)
            {
                if (threadShouldExit()) return;
                const int n = (int) jmin<int64> (chunk, rd.lengthInSamples - pos);
                buf.clear();
                if (! rd.read (&buf, 0, n, pos, true, nch > 1)) { errors << it.file.getFileName() << " (leitura)\n"; break; }
                deEsser.process (buf.getArrayOfWritePointers(), nch, 0, n, *it.curve, (double) pos, 1.0);
                for (int c = 0; c < nch; ++c)
                {
                    auto* d = buf.getWritePointer (c);
                    for (int i = 0; i < n; ++i)
                        d[i] *= it.curve->gainAtSample ((double) (pos + i)) * outGain;
                }
                writer->writeFromAudioSampleBuffer (buf, 0, n);
                doneAll += n;
                setProgress ((double) doneAll / (double) jmax<int64> (1, totalAll));
            }
            written << it.file.getFullPathName() << "\n";
        }
    }

    void threadComplete (bool cancelled) override
    {
        items.clear();   // leitores ARA sao destruidos na thread de mensagens
        String msg = cancelled ? String ("Exportacao cancelada.") : ("Arquivos exportados:\n" + written);
        if (errors.isNotEmpty()) msg << "\nErros:\n" << errors;
        AlertWindow::showAsync (MessageBoxOptions().withIconType (MessageBoxIconType::InfoIcon)
                                    .withTitle ("Export WAV").withMessage (msg).withButton ("OK"), nullptr);
        delete this;
    }

private:
    std::vector<Item> items;
    float outGain;
    String written, errors;
};
}

//==============================================================================
//  Editor
//==============================================================================
VXEditor::VXEditor (VXProcessor& p)
    : AudioProcessorEditor (&p),
      AudioProcessorEditorARAExtension (&p),
      proc (p),
      view (p),
      speed        (p.apvts, ParamID::speed,        "Speed"),
      smoothing    (p.apvts, ParamID::smoothing,    "Smoothing"),
      catchPeaks   (p.apvts, ParamID::catchPeaks,   "Catch Peaks"),
      range        (p.apvts, ParamID::range,        "Range"),
      targetLevel  (p.apvts, ParamID::target,       "Target"),
      floorKnob    (p.apvts, ParamID::floorDb,      "Noisefloor"),
      output       (p.apvts, ParamID::output,       "Output"),
      gateThresh   (p.apvts, ParamID::gateThresh,   "Threshold"),
      gateRed      (p.apvts, ParamID::gateRed,      "Reduction"),
      gateMinLen   (p.apvts, ParamID::gateMinLen,   "Min Length"),
      gateClose    (p.apvts, ParamID::gateClose,    "Close Speed"),
      gateOpen     (p.apvts, ParamID::gateOpen,     "Open Speed"),
      breathRed    (p.apvts, ParamID::breathRed,    "Reduction"),
      breathThresh (p.apvts, ParamID::breathThresh, "Threshold"),
      breathTrans  (p.apvts, ParamID::breathTrans,  "Transition"),
      sibThresh    (p.apvts, ParamID::sibThresh,    "Threshold"),
      sibTrans     (p.apvts, ParamID::sibTrans,     "Transition"),
      sibSpectral  (p.apvts, ParamID::sibSpectral,  "Spectral")
{
    setLookAndFeel (&lnf);

    addAndMakeVisible (view);
    view.onEditsChanged = [this] { updateEnablement(); proc.markDirty(); };

    // ---- abas das secoes
    const char* tabNames[numSections]  = { "GATE", "DYNAMICS", "BREATH", "SIBILANCE" };
    const char* powerIDs[numSections]  = { ParamID::gateOn, ParamID::dynOn, ParamID::breathOn, ParamID::sibOn };
    for (int i = 0; i < numSections; ++i)
    {
        tabs[i].setButtonText (tabNames[i]);
        tabs[i].setClickingTogglesState (true);
        tabs[i].setRadioGroupId (4711);
        tabs[i].onClick = [this, i] { showSection (i); };
        addAndMakeVisible (tabs[i]);

        power[i].setClickingTogglesState (true);
        power[i].setTooltip ("Liga/desliga esta secao");
        powerAtt[i] = std::make_unique<AudioProcessorValueTreeState::ButtonAttachment> (p.apvts, powerIDs[i], power[i]);
        addChildComponent (power[i]);
    }

    // ---- combos
    addCombo (targetBox,     { "LUFS-I Match", "LUFS-I Custom", "LUFS-S Match", "LUFS-S Custom" }, ParamID::targetMode, targetAtt, "TARGET", dynSec);
    addCombo (floorBox,      { "Auto", "Custom" }, ParamID::floorMode,  floorAtt,      "NOISEFLOOR", dynSec);
    addCombo (gateModeBox,   { "Vocal Detection", "Threshold Based" }, ParamID::gateMode, gateModeAtt, "MODE", gateSec);
    addCombo (breathModeBox, { "Auto", "Custom" }, ParamID::breathMode, breathModeAtt, "THRESHOLD", breathSec);
    addCombo (sibModeBox,    { "Auto", "Custom" }, ParamID::sibMode,    sibModeAtt,    "THRESHOLD", sibSec);
    targetBox.setTooltip ("Match: mantem a loudness original (integrada ou short-term). Custom: define o alvo em LUFS.");
    floorBox.setTooltip ("Audio abaixo do noisefloor (pausas, respiros, ruido de sala) nao e levantado.");
    gateModeBox.setTooltip ("Vocal Detection: detecta a voz sozinho. Threshold Based: usa o knob Threshold.");

    // ---- knobs por secao
    for (auto* k : { &speed, &smoothing, &catchPeaks, &range, &targetLevel, &floorKnob })   sectionComps.push_back ({ k, dynSec });
    for (auto* k : { &gateThresh, &gateRed, &gateMinLen, &gateClose, &gateOpen })           sectionComps.push_back ({ k, gateSec });
    for (auto* k : { &breathRed, &breathThresh, &breathTrans })                              sectionComps.push_back ({ k, breathSec });
    for (auto* k : { &sibThresh, &sibTrans, &sibSpectral })                                  sectionComps.push_back ({ k, sibSec });
    for (auto& c : sectionComps) addChildComponent (*c.first);
    addAndMakeVisible (output);

    speed.slider.setTooltip ("Velocidade de reacao do nivelamento. Fala: mais rapido. Canto: mais lento.");
    smoothing.slider.setTooltip ("Suaviza a curva de ganho para as mudancas ficarem inaudiveis.");
    catchPeaks.slider.setTooltip ("Reducao extra em picos rapidos que o nivelamento suave nao pega.");
    range.slider.setTooltip ("Maximo de ganho/atenuacao automaticos.");
    output.slider.setTooltip ("Ganho de saida.");
    gateThresh.slider.setTooltip ("So no modo Threshold Based: abaixo deste nivel e pausa.");
    gateRed.slider.setTooltip ("Quanto as pausas sao abaixadas (-inf = silencio).");
    gateMinLen.slider.setTooltip ("Pausas mais curtas que isto nao sao tocadas.");
    gateClose.slider.setTooltip ("Fade de entrada da pausa (acontece dentro da pausa, nao na voz).");
    gateOpen.slider.setTooltip ("Fade de saida da pausa, antes da voz voltar.");
    breathRed.slider.setTooltip ("Reducao fixa de cada respiro.");
    breathThresh.slider.setTooltip ("Respiros mais altos que isto sao reduzidos tambem pelo excesso (modo Custom).");
    breathTrans.slider.setTooltip ("Duracao das transicoes da reducao.");
    sibThresh.slider.setTooltip ("Sibilantes acima deste nivel sao reduzidos (modo Custom).");
    sibTrans.slider.setTooltip ("Duracao das transicoes ao redor de cada sibilante.");
    sibSpectral.slider.setTooltip ("Parte espectral: corta so as frequencias mais agudas do S. 0% = so reducao de nivel.");

    // ---- barra de ferramentas
    for (auto* b : { &bypassButton, &undoButton, &clearButton, &fitButton, &reanalyseButton, &wavButton, &midiButton,
                     &brushButton, &nodesButton })
        addAndMakeVisible (*b);

    for (auto* b : { &brushButton, &nodesButton })
    {
        b->setClickingTogglesState (true);
        b->setRadioGroupId (4712);
    }
    brushButton.setToggleState (true, dontSendNotification);
    brushButton.onClick = [this] { view.setTool (LevelerView::Tool::brush); repaint(); };
    nodesButton.onClick = [this] { view.setTool (LevelerView::Tool::nodes); repaint(); };
    brushButton.setTooltip ("Pincel (B): arraste para cima/baixo para mudar o ganho de um trecho");
    nodesButton.setTooltip ("Nos (N): clique na curva para criar um ponto e arraste. Alt+clique / duplo clique / botao direito apaga");
    setWantsKeyboardFocus (true);

    bypassButton.setClickingTogglesState (true);
    bypassAtt = std::make_unique<AudioProcessorValueTreeState::ButtonAttachment> (p.apvts, ParamID::bypass, bypassButton);
    undoButton.onClick      = [this] { view.undo(); };
    clearButton.onClick     = [this] { view.clearEdits(); };
    fitButton.onClick       = [this] { view.fitView(); };
    reanalyseButton.onClick = [this] { if (auto* dc = proc.getDocController()) dc->reanalyseAll(); };
    wavButton.onClick       = [this] { exportWav(); };
    midiButton.onClick      = [this] { exportMidi(); };
    undoButton.setTooltip ("Desfaz a ultima edicao manual");
    clearButton.setTooltip ("Remove todas as edicoes manuais dos clipes (os ajustes continuam)");
    reanalyseButton.setTooltip ("Descarta a analise e le o audio de novo");
    wavButton.setTooltip ("Exporta a voz processada (WAV 24 bits) de cada clipe");
    midiButton.setTooltip ("Exporta a automacao de volume como MIDI CC7 (-15 dB = 0, 0 dB = 63, +15 dB = 127)");

    // ARA exige editor redimensionavel
    setResizable (true, true);
    setResizeLimits (1040, 660, 2600, 1600);
    setSize (1120, 720);

    tabs[dynSec].setToggleState (true, dontSendNotification);
    showSection (dynSec);
    updateEnablement();
    timerCallback();
    startTimerHz (10);
}

VXEditor::~VXEditor()
{
    stopTimer();
    setLookAndFeel (nullptr);
}

bool VXEditor::keyPressed (const KeyPress& key)
{
    const auto c = CharacterFunctions::toLowerCase (key.getTextCharacter());
    if (c == 'b') { brushButton.setToggleState (true, sendNotification); view.setTool (LevelerView::Tool::brush); repaint(); return true; }
    if (c == 'n') { nodesButton.setToggleState (true, sendNotification); view.setTool (LevelerView::Tool::nodes); repaint(); return true; }
    if (key == KeyPress ('z', ModifierKeys::commandModifier, 0)) { view.undo(); return true; }
    return false;
}

void VXEditor::addCombo (ComboBox& box, const StringArray& items, const char* paramID,
                         std::unique_ptr<AudioProcessorValueTreeState::ComboBoxAttachment>& att,
                         const String& label, int section)
{
    box.addItemList (items, 1);
    addChildComponent (box);
    att = std::make_unique<AudioProcessorValueTreeState::ComboBoxAttachment> (proc.apvts, paramID, box);
    comboRows.push_back ({ &box, label, section });
    sectionComps.push_back ({ &box, section });
}

void VXEditor::showSection (int index)
{
    currentSection = index;
    for (auto& c : sectionComps) c.first->setVisible (c.second == index);
    for (int i = 0; i < numSections; ++i) power[i].setVisible (i == index);
    resized();
    repaint();
}

void VXEditor::updateEnablement()
{
    targetLevel.setEnabled (targetBox.getSelectedItemIndex() == 1 || targetBox.getSelectedItemIndex() == 3);
    floorKnob.setEnabled (floorBox.getSelectedItemIndex() == 1);
    gateThresh.setEnabled (gateModeBox.getSelectedItemIndex() == 1);
    breathThresh.setEnabled (breathModeBox.getSelectedItemIndex() == 1);
    sibThresh.setEnabled (sibModeBox.getSelectedItemIndex() == 1);

    // aba com secao desligada fica com texto apagado
    for (int i = 0; i < numSections; ++i)
    {
        const bool on = power[i].getToggleState();
        power[i].setButtonText (on ? "ON" : "OFF");
        tabs[i].setColour (TextButton::textColourOffId, on ? Silver::navy : Silver::steel.withAlpha (0.6f));
    }

    const bool ara = proc.isBoundToARA();
    undoButton.setEnabled (ara && view.canUndo());
    clearButton.setEnabled (ara);
    fitButton.setEnabled (ara);
    reanalyseButton.setEnabled (ara);
    wavButton.setEnabled (ara);
    midiButton.setEnabled (ara);
}

void VXEditor::timerCallback()
{
    updateEnablement();

    const bool ara = proc.isBoundToARA();
    modeText = ara ? "ARA 2" : "REALTIME";

    if (auto* dc = proc.getDocController())
    {
        dc->scheduleAnalyses();
        const int regions = proc.getRenderer() != nullptr ? (int) proc.getRenderer()->getPlaybackRegions().size() : 0;
        diagText = "renderer: " + String (proc.getRenderer() != nullptr ? "sim" : "nao")
                 + " | clipes: " + String (regions) + " | " + dc->getDiagnostics();
    }
    else
    {
        diagText = ara ? "ARA ligado, mas sem document controller"
                       : "ARA NAO ativo - ver Preferences > Plug-ins > ARA";
    }

    if (ara)
    {
        nGate = nBreath = nSib = 0;
        if (auto c = view.getFocusCurve())
        {
            inLufs = c->inputLufsI; outLufs = c->outputLufsI; floorVal = c->floorDb; targetVal = c->targetDb;
            gainNow = c->gainDbAtTime (proc.playheadSeconds.load());
            if (auto* r = proc.getRenderer(); r != nullptr && proc.playing.load())
                gainNow = r->currentGainDb.load();
        }
        for (auto& l : view.getLanes())
            if (auto c = proc.getCurveFor (l.mod))
                for (auto& r : c->regions)
                    if (r.enabled)
                    {
                        if (r.type == vx::RegionType::gate) ++nGate;
                        else if (r.type == vx::RegionType::breath) ++nBreath;
                        else ++nSib;
                    }

        int analysing = 0;
        for (auto& l : view.getLanes()) if (l.source->getState() == VXAudioSource::analysing) ++analysing;
        statusText = analysing > 0 ? "Analisando " + String (analysing) + " clipe(s)..." : String();
        if (auto* r = proc.getRenderer(); r != nullptr && r->sampleRateMismatch.load())
            statusText = "Atencao: taxa de amostragem do clipe difere do projeto";
    }
    else
    {
        auto& rt = proc.getRealtime();
        inLufs = rt.getInputShortLufs(); outLufs = rt.getInputShortLufs() + proc.realtimeGainDb.load();
        targetVal = rt.getTargetDb();
        gainNow = proc.realtimeGainDb.load();
        statusText = "Sem ARA: so o Dynamics funciona (tempo real)";
    }
    repaint (headerArea);
    repaint (statsArea);
    repaint (getLocalBounds().removeFromBottom (20));
}

void VXEditor::paint (Graphics& g)
{
    ColourGradient bg (Silver::bgTop, 0.0f, 0.0f, Silver::bgBottom, 0.0f, (float) getHeight(), false);
    g.setGradientFill (bg);
    g.fillAll();

    // cabecalho
    {
        auto h = headerArea.toFloat();
        ColourGradient hg (Colours::white.withAlpha (0.9f), 0.0f, h.getY(), Silver::panelDark, 0.0f, h.getBottom(), false);
        g.setGradientFill (hg);
        g.fillRect (h);
        g.setColour (Silver::border);
        g.drawHorizontalLine (headerArea.getBottom() - 1, 0.0f, (float) getWidth());

        auto t = headerArea.reduced (16, 0);
        g.setColour (Silver::navy);
        g.setFont (FontOptions (24.0f, Font::bold));
        const String name = "VX_Leveler";
        g.drawText (name, t, Justification::centredLeft);
        const int w1 = GlyphArrangement::getStringWidthInt (g.getCurrentFont(), name);
        g.setColour (Silver::steel);
        g.setFont (FontOptions (24.0f));
        g.drawText ("SILVER", t.withTrimmedLeft (w1 + 8), Justification::centredLeft);
        g.setFont (FontOptions (12.0f));
        g.drawText ("vocal leveler", t.withTrimmedLeft (w1 + 100).withTrimmedTop (6), Justification::centredLeft);

        auto pill = Rectangle<float> ((float) headerArea.getRight() - 120.0f, h.getCentreY() - 12.0f, 100.0f, 24.0f);
        g.setColour (modeText == "ARA 2" ? Silver::navyMid : Silver::steel);
        g.fillRoundedRectangle (pill, 12.0f);
        g.setColour (Colours::white);
        g.setFont (FontOptions (13.0f, Font::bold));
        g.drawText (modeText, pill, Justification::centred);

        g.setColour (Silver::navyMid);
        g.setFont (FontOptions (13.0f));
        g.drawText (statusText, headerArea.withTrimmedRight (140).withTrimmedLeft (360), Justification::centredRight);
    }

    // painel lateral
    SilverLookAndFeel::drawMetalPanel (g, sideArea.toFloat());
    {
        const char* titles[numSections] = { "GATE", "DYNAMICS", "BREATH", "SIBILANCE" };
        g.setColour (Silver::navy);
        g.setFont (FontOptions (15.0f, Font::bold));
        g.drawText (titles[currentSection], titleArea, Justification::centredLeft);
        g.setColour (Silver::border);
        g.drawHorizontalLine (titleArea.getBottom() + 3, (float) titleArea.getX(), (float) titleArea.getRight());

        g.setColour (Silver::navy);
        g.setFont (FontOptions (12.5f, Font::bold));
        for (auto& row : comboRows)
            if (row.section == currentSection)
                g.drawText (row.label, row.box->getBounds().withX (sideArea.getX() + 14).withWidth (96), Justification::centredLeft);

        const char* desc[numSections] = {
            "Abaixa as pausas entre as frases. Os fades acontecem dentro da pausa, nunca na voz.",
            "Nivela a voz olhando o take inteiro. Abaixo do noisefloor nada e levantado.",
            "Cada respiro e abaixado pelo valor fixo; os mais altos que o threshold, tambem pelo excesso.",
            "De-esser: reduz o nivel dos S, X e T e corta as frequencias mais agudas de cada um." };
        if (! descArea.isEmpty())
        {
            g.setColour (Silver::steel);
            g.setFont (FontOptions (12.0f));
            g.drawFittedText (desc[currentSection], descArea, Justification::topLeft, 4);
        }
    }

    // estatisticas
    {
        auto r = statsArea.toFloat();
        g.setColour (Colours::white.withAlpha (0.6f));
        g.fillRoundedRectangle (r, 6.0f);
        g.setColour (Silver::border);
        g.drawRoundedRectangle (r.reduced (0.5f), 6.0f, 1.0f);

        auto fmt = [] (float v, const char* unit) { return v <= -99.0f ? String ("--") : String (v, 1) + " " + unit; };
        const bool ara = proc.isBoundToARA();
        const std::pair<String, String> rows[] = {
            { ara ? "Entrada LUFS-I" : "Entrada LUFS-S", fmt (inLufs, "") },
            { ara ? "Saida LUFS-I"   : "Saida LUFS-S",   fmt (outLufs, "") },
            { "Alvo",                fmt (targetVal, "LUFS") },
            { "Noisefloor",          ara ? fmt (floorVal, "dB") : String ("auto") },
            { "Pausas / Resp. / S",  ara ? String (nGate) + " / " + String (nBreath) + " / " + String (nSib) : String ("--") },
            { "Ganho agora",         (gainNow > 0 ? "+" : "") + String (gainNow, 1) + " dB" } };

        auto rr = statsArea.reduced (10, 5);
        const int rh = rr.getHeight() / 6;
        for (auto& row : rows)
        {
            auto line = rr.removeFromTop (rh);
            g.setColour (Silver::steel);
            g.setFont (FontOptions (12.5f));
            g.drawText (row.first, line, Justification::centredLeft);
            g.setColour (Silver::navy);
            g.setFont (FontOptions (13.5f, Font::bold));
            g.drawText (row.second, line, Justification::centredRight);
        }
    }

    // rodape: dicas + diagnostico
    g.setColour (Silver::steel);
    g.setFont (FontOptions (11.5f));
    auto hint = getLocalBounds().removeFromBottom (20).withTrimmedLeft (sideArea.getRight() + 10).withTrimmedRight (12);
    g.drawText (view.getTool() == LevelerView::Tool::brush
                    ? "Pincel: arraste = ganho  |  Shift+roda: largura  |  Ctrl+roda: zoom  |  faixa: liga/desliga regiao"
                    : "Nos: clique = novo ponto, arraste = move (Shift = so vertical)  |  Alt+clique/duplo clique: apaga",
                hint.removeFromLeft (hint.getWidth() / 2), Justification::centredLeft, true);
    g.setColour (diagText.startsWith ("ARA NAO") ? Silver::contrast : Silver::navyMid);
    g.drawText (diagText, hint, Justification::centredRight, true);
}

void VXEditor::resized()
{
    auto r = getLocalBounds();
    headerArea = r.removeFromTop (52);
    r.removeFromBottom (20);
    r.reduce (10, 10);

    sideArea = r.removeFromLeft (300);
    r.removeFromLeft (10);

    // barra de ferramentas
    toolbarArea = r.removeFromTop (32);
    {
        auto t = toolbarArea;
        const int bw = 92;
        bypassButton.setBounds (t.removeFromLeft (80).reduced (2));
        t.removeFromLeft (8);
        undoButton.setBounds (t.removeFromLeft (62).reduced (2));
        clearButton.setBounds (t.removeFromLeft (bw).reduced (2));
        fitButton.setBounds (t.removeFromLeft (46).reduced (2));
        reanalyseButton.setBounds (t.removeFromLeft (bw).reduced (2));
        t.removeFromLeft (10);
        brushButton.setBounds (t.removeFromLeft (70).reduced (2));
        nodesButton.setBounds (t.removeFromLeft (58).reduced (2));
        midiButton.setBounds (t.removeFromRight (bw).reduced (2));
        wavButton.setBounds (t.removeFromRight (bw).reduced (2));
    }
    r.removeFromTop (8);
    view.setBounds (r);

    // ---- painel lateral
    auto s = sideArea.reduced (12, 10);

    auto tabRow = s.removeFromTop (26);
    const int tw = tabRow.getWidth() / numSections;
    for (int i = 0; i < numSections; ++i)
        tabs[i].setBounds ((i == numSections - 1 ? tabRow : tabRow.removeFromLeft (tw)).reduced (1, 0));
    s.removeFromTop (10);

    titleArea = s.removeFromTop (24);
    for (int i = 0; i < numSections; ++i)
        power[i].setBounds (titleArea.withTrimmedLeft (titleArea.getWidth() - 56).reduced (0, 1));
    titleArea = titleArea.withTrimmedRight (60);
    s.removeFromTop (10);

    // parte de baixo: Output + estatisticas (sempre visiveis)
    statsArea = s.removeFromBottom (126);
    s.removeFromBottom (4);
    {
        auto outRow = s.removeFromBottom (88);
        output.setBounds (outRow.withSizeKeepingCentre (outRow.getWidth() / 3, outRow.getHeight()));
    }
    s.removeFromBottom (4);

    // conteudo da secao
    for (auto& row : comboRows)
        if (row.section == currentSection)
        {
            row.box->setBounds (s.removeFromTop (28).withTrimmedLeft (100));
            s.removeFromTop (6);
        }
    s.removeFromTop (6);

    auto placeRow = [&] (std::initializer_list<LabeledKnob*> knobs)
    {
        auto line = s.removeFromTop (88);
        const int kw = line.getWidth() / 3;
        line.removeFromLeft ((3 - (int) knobs.size()) * kw / 2);
        for (auto* k : knobs) k->setBounds (line.removeFromLeft (kw));
        s.removeFromTop (4);
    };

    switch (currentSection)
    {
        case gateSec:   placeRow ({ &gateThresh, &gateRed, &gateMinLen }); placeRow ({ &gateClose, &gateOpen }); break;
        case dynSec:    placeRow ({ &speed, &smoothing, &catchPeaks });   placeRow ({ &targetLevel, &floorKnob, &range }); break;
        case breathSec: placeRow ({ &breathRed, &breathThresh, &breathTrans }); break;
        case sibSec:    placeRow ({ &sibThresh, &sibTrans, &sibSpectral }); break;
        default: break;
    }
    s.removeFromTop (6);
    descArea = s.getHeight() > 30 ? s.withHeight (jmin (70, s.getHeight())).reduced (2, 0) : Rectangle<int>();
}

//------------------------------------------------------------------------------
void VXEditor::exportWav()
{
    if (view.getLanes().empty()) return;

    chooser = std::make_unique<FileChooser> ("Pasta para exportar a voz processada",
                                             File::getSpecialLocation (File::userDocumentsDirectory));
    chooser->launchAsync (FileBrowserComponent::openMode | FileBrowserComponent::canSelectDirectories,
                          [this] (const FileChooser& fc)
    {
        const auto dir = fc.getResult();
        if (! dir.isDirectory()) return;

        std::vector<WavExportJob::Item> items;
        std::set<VXAudioModification*> done;
        for (auto& l : view.getLanes())
        {
            if (! done.insert (l.mod).second) continue;
            auto curve = proc.getCurveFor (l.mod);
            if (curve == nullptr || ! l.source->isSampleAccessEnabled()) continue;

            WavExportJob::Item it;
            it.reader = std::make_unique<ARAAudioSourceReader> (l.source);
            if (! it.reader->isValid()) continue;
            it.curve = curve;
            it.file = dir.getNonexistentChildFile (File::createLegalFileName (l.mod->getDisplayName().upToLastOccurrenceOf (".", false, false))
                                                   + "_VXLeveled", ".wav", false);
            items.push_back (std::move (it));
        }

        if (items.empty())
        {
            AlertWindow::showAsync (MessageBoxOptions().withIconType (MessageBoxIconType::WarningIcon).withTitle ("Export WAV")
                                        .withMessage ("Nenhum clipe analisado para exportar.").withButton ("OK"), nullptr);
            return;
        }

        auto* job = new WavExportJob (std::move (items), Decibels::decibelsToGain (proc.apvts.getRawParameterValue (ParamID::output)->load()));
        job->launchThread();
    });
}

void VXEditor::exportMidi()
{
    const auto lanes = view.getLanes();
    if (lanes.empty()) return;

    chooser = std::make_unique<FileChooser> ("Salvar automacao (MIDI CC7)",
                                             File::getSpecialLocation (File::userDocumentsDirectory).getChildFile ("VX_Leveler_automation.mid"),
                                             "*.mid");
    chooser->launchAsync (FileBrowserComponent::saveMode | FileBrowserComponent::canSelectFiles
                              | FileBrowserComponent::warnAboutOverwriting,
                          [this] (const FileChooser& fc)
    {
        auto file = fc.getResult();
        if (file == File()) return;
        file = file.withFileExtension (".mid");

        // 120 BPM, 960 ppq  ->  1 s = 1920 ticks
        const double ticksPerSecond = 1920.0;
        MidiMessageSequence seq;
        seq.addEvent (MidiMessage::tempoMetaEvent (500000), 0.0);

        auto toCC = [] (float db)
        {
            const float v = db < 0.0f ? 63.0f + db * 63.0f / 15.0f : 63.0f + db * 64.0f / 15.0f;
            return jlimit (0, 127, roundToInt (v));
        };

        int last = -1;
        for (auto& l : view.getLanes())
        {
            auto curve = proc.getCurveFor (l.mod);
            if (curve == nullptr) continue;
            const double step = 0.02;
            for (double t = l.playStart; t < l.playEnd; t += step)
            {
                const double srcT = t - l.playStart + l.modStart;
                const int v = toCC (curve->gainDbAtTime (srcT));
                if (v != last)
                {
                    seq.addEvent (MidiMessage::controllerEvent (1, 7, v), jmax (0.0, t) * ticksPerSecond);
                    last = v;
                }
            }
            seq.addEvent (MidiMessage::controllerEvent (1, 7, 63), jmax (0.0, l.playEnd) * ticksPerSecond);
            last = 63;
        }
        seq.updateMatchedPairs();

        MidiFile mf;
        mf.setTicksPerQuarterNote (960);
        mf.addTrack (seq);
        file.deleteFile();
        FileOutputStream out (file);
        const bool ok = out.openedOk() && mf.writeTo (out);
        AlertWindow::showAsync (MessageBoxOptions().withIconType (ok ? MessageBoxIconType::InfoIcon : MessageBoxIconType::WarningIcon)
                                    .withTitle ("Export MIDI")
                                    .withMessage (ok ? "Automacao salva em:\n" + file.getFullPathName() + "\n\n(120 BPM, CC7 no canal 1)"
                                                     : "Nao foi possivel salvar o arquivo.")
                                    .withButton ("OK"), nullptr);
    });
}
