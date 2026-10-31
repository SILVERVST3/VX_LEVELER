// Teste offline do nivelador: "frases" sinteticas com niveis bem diferentes,
// separadas por pausas com ruido de sala. Mede a dispersao do nivel das frases
// antes e depois.
#include "LevelerDSP.h"
#include <cstdio>
#include <random>
#include <vector>
#include <cmath>

int main()
{
    const double fs = 48000.0;
    std::mt19937 rng (1);
    std::normal_distribution<float> nd (0.0f, 1.0f);

    // (nivel dB, duracao s) - frase / pausa alternadas
    const float phraseDb[] = { -12, -30, -18, -8, -26, -15, -22, -10 };
    std::vector<float> x;
    std::vector<std::pair<size_t, size_t>> phrases;
    float lp = 0.0f;
    for (float db : phraseDb)
    {
        const size_t start = x.size();
        const size_t len = (size_t) (fs * 2.0);
        for (size_t i = 0; i < len; ++i)
        {
            // "voz": ruido filtrado com envelope silabico 4 Hz
            lp += 0.2f * (nd (rng) - lp);
            const float syll = 0.55f + 0.45f * std::sin (2.0f * 3.14159f * 4.0f * (float) i / (float) fs);
            x.push_back (lp * syll * std::pow (10.0f, db / 20.0f) * 3.0f);
        }
        phrases.push_back ({ start, x.size() });
        for (size_t i = 0; i < (size_t) (fs * 0.6); ++i) x.push_back (nd (rng) * 0.0005f);   // pausa (~ -66 dB)
    }

    vx::Analyzer an (fs, 1, (int64_t) x.size());
    const float* chans[1] = { x.data() };
    an.process (chans, 1, (int) x.size());
    auto analysis = an.finish();

    vx::Settings st;
    auto curve = vx::computeGainCurve (*analysis, st, nullptr);

    std::printf ("Entrada LUFS-I %.1f  Saida LUFS-I %.1f  piso %.1f  alvo %.1f\n",
                 curve->inputLufsI, curve->outputLufsI, curve->floorDb, curve->targetDb);

    auto rmsDb = [&] (size_t a, size_t b, bool processed)
    {
        double s = 0.0;
        for (size_t i = a; i < b; ++i)
        {
            const float g = processed ? curve->gainAtSample ((double) i) : 1.0f;
            s += (double) (x[i] * g) * (x[i] * g);
        }
        return 10.0 * std::log10 (s / (double) (b - a) + 1e-12);
    };

    double minIn = 1e9, maxIn = -1e9, minOut = 1e9, maxOut = -1e9;
    for (auto& p : phrases)
    {
        const size_t a = p.first + (size_t) (fs * 0.3), b = p.second - (size_t) (fs * 0.3);
        const double i = rmsDb (a, b, false), o = rmsDb (a, b, true);
        std::printf ("  frase: entrada %6.1f dB  -> saida %6.1f dB\n", i, o);
        minIn = std::min (minIn, i); maxIn = std::max (maxIn, i);
        minOut = std::min (minOut, o); maxOut = std::max (maxOut, o);
    }
    std::printf ("Dispersao entrada %.1f dB -> saida %.1f dB\n", maxIn - minIn, maxOut - minOut);

    // pausas nao podem subir
    double pauseIn = rmsDb (phrases[1].second + 4800, phrases[2].first - 4800, false);
    double pauseOut = rmsDb (phrases[1].second + 4800, phrases[2].first - 4800, true);
    std::printf ("Pausa apos frase baixa: %.1f -> %.1f dB\n", pauseIn, pauseOut);

    // modo tempo real
    vx::RealtimeLeveler rt;
    rt.prepare (fs, 1);
    rt.setSettings (st);
    std::vector<float> y (x);
    float* yc[1] = { y.data() };
    for (size_t pos = 0; pos < y.size(); pos += 512)
    {
        float* p[1] = { y.data() + pos };
        rt.process (p, 1, (int) std::min<size_t> (512, y.size() - pos), false);
    }
    (void) yc;
    const size_t lat = (size_t) rt.getLatencySamples();
    minOut = 1e9; maxOut = -1e9;
    for (auto& p : phrases)
    {
        const size_t a = p.first + (size_t) (fs * 0.8) + lat, b = p.second - (size_t) (fs * 0.2) + lat;
        double s = 0.0; for (size_t i = a; i < b; ++i) s += (double) y[i] * y[i];
        const double o = 10.0 * std::log10 (s / (double) (b - a) + 1e-12);
        minOut = std::min (minOut, o); maxOut = std::max (maxOut, o);
    }
    std::printf ("Tempo real: dispersao saida %.1f dB (latencia %zu amostras)\n", maxOut - minOut, lat);
    return 0;
}
