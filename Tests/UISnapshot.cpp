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

    // sinal de teste: frases com niveis diferentes
    std::vector<float> x;
    std::mt19937 rng (1);
    std::normal_distribution<float> nd (0.0f, 1.0f);
    float lp = 0.0f;
    const float levels[] = { -14, -30, -20, -10, -26, -16 };
    for (float db : levels)
    {
        for (int i = 0; i < 48000 * 2; ++i)
        {
            lp += 0.2f * (nd (rng) - lp);
            const float syl = 0.5f + 0.5f * std::abs (std::sin (3.14159f * 3.0f * (float) i / 48000.0f));
            x.push_back (lp * syl * std::pow (10.0f, db / 20.0f) * 3.0f);
        }
        for (int i = 0; i < 48000 * 6 / 10; ++i) x.push_back (nd (rng) * 0.0004f);
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
    auto curve = vx::computeGainCurve (*analysis, vx::Settings(), &edits);

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

    ed.reset();
    proc.reset();
    return 0;
}
