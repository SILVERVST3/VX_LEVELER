// Teste offline: "voz" harmonica com frases em niveis diferentes, respiros entre
// frases e sibilantes fortes dentro das frases.
#include "LevelerDSP.h"
#include <cstdio>
#include <random>
#include <vector>
#include <cmath>
#include <cstdlib>

static const double fs = 48000.0;

int main()
{
    std::mt19937 rng (1);
    std::normal_distribution<float> nd (0.0f, 1.0f);
    const float phraseDb[] = { -12, -30, -18, -8, -26, -15, -22, -10 };

    std::vector<float> x;
    std::vector<std::pair<size_t, size_t>> phrases, breaths, sibs;
    double ph = 0.0;
    vx::Biquad bpBreath, hpSib;
    bpBreath.setBandpass (fs, 2500.0, 0.7);
    hpSib.setHighpass (fs, 5500.0, 0.7);

    auto addNoise = [&] (size_t n, float amp) { for (size_t i = 0; i < n; ++i) x.push_back (nd (rng) * amp); };

    for (float db : phraseDb)
    {
        const float a = std::pow (10.0f, db / 20.0f);
        const size_t start = x.size();
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
            if (syl == 2)   // "s" forte
            {
                const size_t s0 = x.size();
                for (size_t i = 0; i < (size_t) (fs * 0.09); ++i) x.push_back ((float) hpSib.process (nd (rng)) * a * 1.6f);
                sibs.push_back ({ s0, x.size() });
            }
        }
        phrases.push_back ({ start, x.size() });
        addNoise ((size_t) (fs * 0.25), 0.0004f);
        const size_t b0 = x.size();   // respiro
        for (size_t i = 0; i < (size_t) (fs * 0.35); ++i)
        {
            const float env = (float) std::sin (3.14159 * (double) i / (fs * 0.35));
            x.push_back ((float) bpBreath.process (nd (rng)) * 0.05f * env);
        }
        breaths.push_back ({ b0, x.size() });
        addNoise ((size_t) (fs * 0.25), 0.0004f);
    }

    vx::Analyzer an (fs, 1, (int64_t) x.size());
    const float* chans[1] = { x.data() };
    an.process (chans, 1, (int) x.size());
    auto analysis = an.finish();

    vx::Settings st;
    st.gateOn = true; st.gateReduction = -20.0f;
    auto curve = vx::computeGainCurve (*analysis, st, nullptr);

    std::printf ("LUFS-I entrada %.1f saida %.1f | piso %.1f | pausas %d respiros %d sibilantes %d (esperado: %zu respiros, %zu sibilantes)\n",
                 curve->inputLufsI, curve->outputLufsI, curve->floorDb, curve->numGate, curve->numBreath, curve->numSib,
                 breaths.size(), sibs.size());

    auto rmsDb = [&] (size_t a, size_t b, bool processed)
    {
        double s = 0.0;
        for (size_t i = a; i < b; ++i) { const float g = processed ? curve->gainAtSample ((double) i) : 1.0f; s += (double) (x[i] * g) * (x[i] * g); }
        return 10.0 * std::log10 (s / (double) (b - a) + 1e-12);
    };

    double mn = 1e9, mx = -1e9, mnI = 1e9, mxI = -1e9;
    for (auto& p : phrases)
    {
        const size_t a = p.first + (size_t) (fs * 0.2), b = p.first + (size_t) (fs * 0.55);
        const double i = rmsDb (a, b, false), o = rmsDb (a, b, true);
        mnI = std::min (mnI, i); mxI = std::max (mxI, i); mn = std::min (mn, o); mx = std::max (mx, o);
    }
    std::printf ("Frases: dispersao %.1f dB -> %.1f dB\n", mxI - mnI, mx - mn);

    double br = 0; for (auto& b : breaths) br += rmsDb (b.first, b.second, true) - rmsDb (b.first, b.second, false);
    std::printf ("Respiros: variacao media %.1f dB\n", br / breaths.size());
    double sb = 0; for (auto& s : sibs) sb += rmsDb (s.first, s.second, true) - rmsDb (s.first + 0, s.second, false);
    std::printf ("Sibilantes: variacao media (banda larga) %.1f dB | espectral: %s\n", sb / sibs.size(), curve->hasSpectral ? "sim" : "nao");

    for (auto& r : curve->regions)
        if (r.type == vx::RegionType::sibilance)
        { std::printf ("  sib %.2f-%.2f s  larga %.1f  espectral -%.1f dB\n", r.startFrame * 0.01, r.endFrame * 0.01, r.depthDb, r.extraDb); break; }

    if (getenv ("VXDBG"))
    {
        for (auto& b : breaths) std::printf ("  respiro real %.2f-%.2f\n", b.first / fs, b.second / fs);
        for (auto& r : curve->regions) if (r.type == vx::RegionType::breath) std::printf ("  respiro det %.2f-%.2f %.1f dB\n", r.startFrame * 0.01, r.endFrame * 0.01, r.depthDb);
        int f0 = (int) (breaths[0].first / 480), f1 = (int) (breaths[0].second / 480);
        for (int f = f0 - 3; f < f1 + 3; f += 3) std::printf ("   f%d v=%.2f zcr=%.0f hf=%.2f ms=%.1f g=%.1f\n", f, analysis->voicing[f], analysis->zcr[f], analysis->hfMs[f] / (analysis->totMs[f] + 1e-12f), vx::msToLufs (analysis->ms[f]), curve->gainDb[f]);
    }
    // de-esser espectral roda?
    vx::SpectralDeEsser de; de.prepare (fs, 1);
    std::vector<float> y (x);
    float* yc[1] = { y.data() };
    de.process (yc, 1, 0, (int) y.size(), *curve, 0.0, 1.0);
    double e0 = 0, e1 = 0; for (auto& s : sibs) for (size_t i = s.first; i < s.second; ++i) { e0 += x[i] * x[i]; e1 += y[i] * y[i]; }
    std::printf ("De-esser espectral nos sibilantes: %.1f dB\n", 10 * std::log10 (e1 / e0));
    return 0;
}
