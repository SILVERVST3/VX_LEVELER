// Gera PNGs do editor para conferir o visual - so para desenvolvimento.
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include "PluginEditor.h"
#include <cmath>
#include <random>

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter();

static void save (juce::Component& c, const juce::String& path)
{
    auto img = c.createComponentSnapshot (c.getLocalBounds(), true, 1.0f);
    juce::File out (path);
    out.deleteFile();
    juce::FileOutputStream os (out);
    juce::PNGImageFormat().writeImageToStream (img, os);
}

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI init;
    std::unique_ptr<juce::AudioProcessor> proc (createPluginFilter());
    proc->setPlayConfigDetails (2, 2, 48000.0, 512);
    proc->prepareToPlay (48000.0, 512);

    // sinal de teste: voz harmonica, respiros e sibilantes
    std::vector<float> x;
    std::mt19937 rng (1);
    std::normal_distribution<float> nd (0.0f, 1.0f);
    {
        const double fs = 48000.0;
        const float levels[] = { -14, -30, -20, -10, -26, -16 };
        vx::Biquad bpBreath, hpSib;
        bpBreath.setBandpass (fs, 2500.0, 0.7);
        hpSib.setHighpass (fs, 5500.0, 0.7);
        double ph = 0.0;
        for (float db : levels)
        {
            const float a = std::pow (10.0f, db / 20.0f);
            for (int syl = 0; syl < 6; ++syl)
            {
                const size_t len = (size_t) (fs * 0.28);
                const double f0 = 140.0 + 30.0 * syl;
                for (size_t i = 0; i < len; ++i)
                {
                    ph += 2.0 * 3.14159265 * f0 / fs;
                    double v = 0.0;
                    for (int h = 1; h <= 12; ++h) v += std::sin (ph * h) / h;
                    const float env = (float) std::sin (3.14159 * (double) i / (double) len);
                    x.push_back ((float) v * 0.5f * a * (0.3f + 0.7f * env) + nd (rng) * 0.0003f);
                }
                if (syl == 2)
                    for (size_t i = 0; i < (size_t) (fs * 0.09); ++i) x.push_back ((float) hpSib.process (nd (rng)) * a * 1.6f);
            }
            for (size_t i = 0; i < (size_t) (fs * 0.25); ++i) x.push_back (nd (rng) * 0.0004f);
            for (size_t i = 0; i < (size_t) (fs * 0.35); ++i)
                x.push_back ((float) bpBreath.process (nd (rng)) * 0.05f * (float) std::sin (3.14159 * (double) i / (fs * 0.35)));
            for (size_t i = 0; i < (size_t) (fs * 0.25); ++i) x.push_back (nd (rng) * 0.0004f);
        }
    }

    juce::AudioBuffer<float> buf (2, 512);
    juce::MidiBuffer midi;
    for (size_t pos = 0; pos + 512 <= x.size(); pos += 512)
    {
        for (int i = 0; i < 512; ++i) { buf.setSample (0, i, x[pos + (size_t) i]); buf.setSample (1, i, x[pos + (size_t) i]); }
        proc->processBlock (buf, midi);
    }

    std::unique_ptr<juce::AudioProcessorEditor> ed (proc->createEditor());
    ed->setSize (1100, 660);
    save (*ed, argc > 1 ? argv[1] : "snap_realtime.png");

    // simula o modo ARA com um clipe analisado
    vx::Analyzer an (48000.0, 1, (int64_t) x.size());
    const float* ch[1] = { x.data() };
    an.process (ch, 1, (int) x.size());
    std::shared_ptr<const vx::Analysis> analysis = an.finish();
    std::vector<float> edits ((size_t) analysis->numFrames(), 0.0f);
    for (int f = 820; f < 1000; ++f) edits[(size_t) f] = -4.0f * (float) std::sin (3.14159 * (f - 820) / 180.0);
    vx::Settings st;
    st.gateOn = true; st.gateReduction = -18.0f;
    auto curve = vx::computeGainCurve (*analysis, st, &edits);
    printf ("dur %.2f s frames %d gate %d breath %d sib %d floor %.1f in %.1f\n", x.size() / 48000.0, analysis->numFrames(), curve->numGate, curve->numBreath, curve->numSib, curve->floorDb, curve->inputLufsI);
    for (auto& r : curve->regions) printf ("  t%d %.2f-%.2f %.1f\n", (int) r.type, r.startFrame * 0.01, r.endFrame * 0.01, r.depthDb);

    for (auto* c : ed->getChildren())
        if (auto* v = dynamic_cast<LevelerView*> (c))
        {
            LevelerView::DrawLane d;
            d.analysis = analysis; d.curve = curve;
            d.playStart = 1.0; d.playEnd = 1.0 + (double) x.size() / 48000.0; d.modStart = 0.0;
            d.name = "Lead Vox take 3";
            v->previewLanes.push_back (d);
        }
    save (*ed, argc > 2 ? argv[2] : "snap_ara.png");

    // capturas das outras abas
    int idx = 0;
    for (auto* name : { "GATE", "BREATH", "SIBILANCE" })
    {
        for (auto* c : ed->getChildren())
            if (auto* b = dynamic_cast<juce::TextButton*> (c))
                if (b->getButtonText() == name && b->onClick) { b->setToggleState (true, juce::dontSendNotification); b->onClick(); }
        save (*ed, juce::String ("/tmp/snap_tab") + juce::String (idx++) + ".png");
    }

    ed.reset();
    proc.reset();
    return 0;
}
