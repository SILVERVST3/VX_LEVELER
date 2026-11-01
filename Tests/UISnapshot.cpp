// Gera PNGs do editor para conferir o visual - so para desenvolvimento.
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include "PluginEditor.h"
#include "VXDocumentController.h"
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
    {   // ROUNDTRIP: analise e estado manual
        auto blk = vxSerialiseAnalysis (*analysis);
        auto back = vxDeserialiseAnalysis (blk.getData(), blk.getSize());
        // compara as curvas calculadas com a analise original e com a recuperada
        bool same = false;
        if (back != nullptr)
        {
            vx::Settings s0; s0.gateOn = true;
            auto c1 = vx::computeGainCurve (*analysis, s0, nullptr);
            auto c2 = vx::computeGainCurve (*back, s0, nullptr);
            float maxDiff = 0.0f;
            for (size_t i = 0; i < c1->gainDb.size(); ++i) maxDiff = std::max (maxDiff, std::abs (c1->gainDb[i] - c2->gainDb[i]));
            printf ("diferenca maxima da curva apos salvar/ler: %.3f dB (regioes %zu vs %zu)\n", maxDiff, c1->regions.size(), c2->regions.size());
            same = maxDiff < 0.1f && c1->regions.size() == c2->regions.size();
        }
        printf ("analise: %d quadros -> %d bytes gzip, ida e volta %s\n", analysis->numFrames(), (int) blk.getSize(), same ? "OK" : "FALHOU");
        VXModState st;
        st.edits.assign (1000, 0.0f); for (int i = 200; i < 260; ++i) st.edits[(size_t) i] = 2.5f;
        st.disabled.push_back ({ 1, 345 });
        st.nodes.push_back ({ 1.25, -3.0f }); st.nodes.push_back ({ 2.5, 4.0f });
        juce::MemoryOutputStream mo; st.write (mo);
        juce::MemoryInputStream mi (mo.getMemoryBlock(), true);
        VXModState st2; const bool ok = st2.read (mi);
        printf ("estado manual: %d bytes, ida e volta %s\n", (int) mo.getDataSize(),
                ok && st2.edits == st.edits && st2.disabled.size() == 1 && st2.disabled[0].startFrame == 345
                   && st2.nodes.size() == 2 && st2.nodes[1].db == 4.0f ? "OK" : "FALHOU");
    }
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

    // NOS_TOOL: seleciona a ferramenta Nos
    for (auto* c : ed->getChildren())
        if (auto* b = dynamic_cast<juce::TextButton*> (c))
            if (b->getButtonText() == juce::String::fromUTF8 ("N\xc3\xb3s")) { b->setToggleState (true, juce::dontSendNotification); b->onClick(); }
    save (*ed, "/tmp/snap_nodes.png");

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
