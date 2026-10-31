#include "LevelerDSP.h"
#include <algorithm>
#include <numeric>

namespace vx
{
static constexpr double kPi = 3.14159265358979323846;

//==============================================================================
void KWeighting::prepare (double fs) noexcept
{
    // Coeficientes BS.1770 para qualquer taxa de amostragem (mesma formulacao do libebur128)
    {
        const double f0 = 1681.974450955533, G = 3.999843853973347, Q = 0.7071752369554196;
        const double K  = std::tan (kPi * f0 / fs);
        const double Vh = std::pow (10.0, G / 20.0);
        const double Vb = std::pow (Vh, 0.4996667741545416);
        const double a0 = 1.0 + K / Q + K * K;
        b1[0] = (Vh + Vb * K / Q + K * K) / a0;
        b1[1] = 2.0 * (K * K - Vh) / a0;
        b1[2] = (Vh - Vb * K / Q + K * K) / a0;
        a1[0] = 1.0;
        a1[1] = 2.0 * (K * K - 1.0) / a0;
        a1[2] = (1.0 - K / Q + K * K) / a0;
    }
    {
        const double f0 = 38.13547087602444, Q = 0.5003270373238773;
        const double K  = std::tan (kPi * f0 / fs);
        const double a0 = 1.0 + K / Q + K * K;
        b2[0] = 1.0; b2[1] = -2.0; b2[2] = 1.0;
        a2[0] = 1.0;
        a2[1] = 2.0 * (K * K - 1.0) / a0;
        a2[2] = (1.0 - K / Q + K * K) / a0;
    }
    reset();
}

//==============================================================================
Analyzer::Analyzer (double sampleRate, int numChannels, int64_t totalSamples)
{
    result = std::make_shared<Analysis>();
    result->sampleRate  = sampleRate;
    result->hopSamples  = std::max (1, (int) std::lround (sampleRate * kHopSeconds));
    result->numSamples  = totalSamples;
    result->numChannels = std::max (1, numChannels);
    const auto frames = (size_t) (totalSamples / result->hopSamples + 1);
    result->ms.reserve (frames);
    result->peak.reserve (frames);

    filters.resize ((size_t) result->numChannels);
    for (auto& f : filters) f.prepare (sampleRate);
}

void Analyzer::process (const float* const* channels, int numChannels, int numSamples)
{
    const int nch = std::min (numChannels, (int) filters.size());
    const int hop = result->hopSamples;

    for (int i = 0; i < numSamples; ++i)
    {
        for (int c = 0; c < nch; ++c)
        {
            const float x = channels[c][i];
            const float y = filters[(size_t) c].process (x);
            accMs += (double) y * (double) y;
            accPeak = std::max (accPeak, std::abs (x));
        }

        if (++inFrame == hop)
        {
            result->ms.push_back ((float) (accMs / hop));
            result->peak.push_back (accPeak);
            accMs = 0.0; accPeak = 0.0f; inFrame = 0;
        }
    }
}

std::shared_ptr<Analysis> Analyzer::finish()
{
    if (inFrame > 0)
    {
        result->ms.push_back ((float) (accMs / inFrame));
        result->peak.push_back (accPeak);
        accMs = 0.0; accPeak = 0.0f; inFrame = 0;
    }
    return result;
}

//==============================================================================
namespace
{
    float percentile (std::vector<float> v, float p)
    {
        if (v.empty()) return kMinDb;
        const auto k = (size_t) std::clamp ((double) p * (double) (v.size() - 1), 0.0, (double) (v.size() - 1));
        std::nth_element (v.begin(), v.begin() + (long) k, v.end());
        return v[k];
    }

    /** media movel centrada (janela de n quadros) usando somas prefixadas */
    struct Prefix
    {
        std::vector<double> s;
        explicit Prefix (const std::vector<float>& x) : s (x.size() + 1, 0.0)
        {
            for (size_t i = 0; i < x.size(); ++i) s[i + 1] = s[i] + (double) x[i];
        }
        double sum (int a, int b) const noexcept   // [a, b)
        {
            const int n = (int) s.size() - 1;
            a = std::clamp (a, 0, n); b = std::clamp (b, 0, n);
            return b > a ? s[(size_t) b] - s[(size_t) a] : 0.0;
        }
    };

    void zeroPhaseOnePole (std::vector<float>& x, double tauSeconds)
    {
        if (x.empty() || tauSeconds <= 0.0) return;
        const float a = (float) std::exp (-kHopSeconds / tauSeconds);
        float y = x.front();
        for (auto& v : x)                     { y = a * y + (1.0f - a) * v; v = y; }
        y = x.back();
        for (auto it = x.rbegin(); it != x.rend(); ++it) { y = a * y + (1.0f - a) * *it; *it = y; }
    }

    /** loudness de janela (n quadros) centrada em cada quadro, com ganho opcional */
    std::vector<float> windowedLoudness (const std::vector<float>& ms, const float* gainDb, int n)
    {
        std::vector<float> weighted (ms);
        if (gainDb != nullptr)
            for (size_t i = 0; i < ms.size(); ++i)
                weighted[i] = ms[i] * std::pow (10.0f, gainDb[i] * 0.1f);

        Prefix p (weighted);
        std::vector<float> out (ms.size());
        const int h = n / 2;
        for (int i = 0; i < (int) ms.size(); ++i)
        {
            const int a = i - h, b = i - h + n;
            const int cnt = std::min (b, (int) ms.size()) - std::max (a, 0);
            out[(size_t) i] = cnt > 0 ? msToLufs (p.sum (a, b) / cnt) : kMinDb;
        }
        return out;
    }
}

float integratedLoudness (const std::vector<float>& ms, const float* gainDb)
{
    // blocos de 400 ms com passo de 100 ms (75% overlap), gates absoluto -70 e relativo -10
    const int blockLen = 40, step = 10;
    if ((int) ms.size() < blockLen)
    {
        double s = 0.0; for (size_t i = 0; i < ms.size(); ++i) s += ms[i] * (gainDb ? std::pow (10.0, gainDb[i] * 0.1) : 1.0);
        return ms.empty() ? kMinDb : msToLufs (s / (double) ms.size());
    }

    std::vector<double> blocks;
    blocks.reserve (ms.size() / step + 1);
    for (int start = 0; start + blockLen <= (int) ms.size(); start += step)
    {
        double s = 0.0;
        for (int i = start; i < start + blockLen; ++i)
            s += (double) ms[(size_t) i] * (gainDb ? std::pow (10.0, gainDb[i] * 0.1) : 1.0);
        blocks.push_back (s / blockLen);
    }

    double sum = 0.0; int cnt = 0;
    for (auto b : blocks) if (msToLufs (b) > -70.0f) { sum += b; ++cnt; }
    if (cnt == 0) return kMinDb;
    const float relGate = msToLufs (sum / cnt) - 10.0f;

    sum = 0.0; cnt = 0;
    for (auto b : blocks) { const float l = msToLufs (b); if (l > -70.0f && l > relGate) { sum += b; ++cnt; } }
    return cnt > 0 ? msToLufs (sum / cnt) : kMinDb;
}

//==============================================================================
std::shared_ptr<GainCurve> computeGainCurve (const Analysis& an, const Settings& st, const std::vector<float>* edits)
{
    auto curve = std::make_shared<GainCurve>();
    curve->sampleRate = an.sampleRate;
    curve->hopSamples = an.hopSamples;

    const int N = an.numFrames();
    curve->autoDb.assign ((size_t) N, 0.0f);
    curve->gainDb.assign ((size_t) N, 0.0f);
    curve->gainLin.assign ((size_t) N, 1.0f);
    if (N == 0) return curve;

    const auto& ms = an.ms;

    // ---------------------------------------------------------------- estatisticas da entrada
    const float inI = integratedLoudness (ms, nullptr);
    curve->inputLufsI = inI;
    if (inI <= -69.0f)   // silencio
    {
        curve->outputLufsI = inI;
        return curve;
    }

    const auto fast  = windowedLoudness (ms, nullptr, 5);     // 50 ms
    const auto block = windowedLoudness (ms, nullptr, 40);    // 400 ms
    const auto shortIn = windowedLoudness (ms, nullptr, 300); // 3 s (LUFS-S)

    // ---------------------------------------------------------------- noisefloor
    float floorDb = st.floorDb;
    if (st.floorAuto)
    {
        std::vector<float> bl;
        bl.reserve (block.size() / 10 + 1);
        for (size_t i = 0; i < block.size(); i += 10) if (block[i] > -70.0f) bl.push_back (block[i]);
        const float p10 = percentile (bl, 0.10f);
        floorDb = std::max (p10 + 6.0f, inI - 30.0f);
        floorDb = std::clamp (floorDb, -75.0f, inI - 12.0f);
    }
    curve->floorDb = floorDb;

    // ---------------------------------------------------------------- deteccao de voz (acima do piso)
    std::vector<uint8_t> active ((size_t) N, 0);
    for (int i = 0; i < N; ++i) active[(size_t) i] = fast[(size_t) i] > floorDb ? 1 : 0;
    const std::vector<uint8_t> rawActive (active);

    {   // dilata 150 ms (hangover) e fecha buracos < 250 ms dentro das frases
        const int hang = 15, gapFill = 25;
        std::vector<uint8_t> d (active);
        int last = -100000;
        for (int i = 0; i < N; ++i) { if (active[(size_t) i]) last = i; if (i - last <= hang) d[(size_t) i] = 1; }
        last = 100000000;
        for (int i = N - 1; i >= 0; --i) { if (active[(size_t) i]) last = i; if (last - i <= hang) d[(size_t) i] = 1; }
        int i = 0;
        while (i < N)
        {
            if (d[(size_t) i]) { ++i; continue; }
            int j = i; while (j < N && ! d[(size_t) j]) ++j;
            if (i > 0 && j < N && (j - i) < gapFill) for (int k = i; k < j; ++k) d[(size_t) k] = 1;
            i = j;
        }
        active.swap (d);
    }

    // ---------------------------------------------------------------- alvo
    float target = st.targetLufs;
    {
        std::vector<float> stAct;
        for (int i = 0; i < N; i += 10) if (active[(size_t) i] && shortIn[(size_t) i] > -70.0f) stAct.push_back (shortIn[(size_t) i]);
        curve->inputShortMax = percentile (stAct, 0.98f);

        switch (st.targetMode)
        {
            case TargetMode::lufsIMatch:  target = inI; break;
            case TargetMode::lufsSMatch:  target = curve->inputShortMax; break;
            case TargetMode::lufsICustom:
            case TargetMode::lufsSCustom: target = st.targetLufs; break;
        }
    }
    curve->targetDb = target;

    // ---------------------------------------------------------------- loudness local "com contexto" (Speed)
    // Media da energia apenas dos quadros com voz numa janela centrada de duracao = Speed.
    const int W = std::max (3, (int) std::lround (st.speedMs * 0.001 / kHopSeconds));
    std::vector<float> msAct ((size_t) N), cnt ((size_t) N);
    for (int i = 0; i < N; ++i) { msAct[(size_t) i] = active[(size_t) i] ? ms[(size_t) i] : 0.0f; cnt[(size_t) i] = active[(size_t) i] ? 1.0f : 0.0f; }
    Prefix pS (msAct), pC (cnt);

    std::vector<float> g ((size_t) N, 0.0f);
    std::vector<uint8_t> has ((size_t) N, 0);
    const float range = std::max (0.0f, st.rangeDb);
    for (int i = 0; i < N; ++i)
    {
        if (! active[(size_t) i]) continue;
        const int a = i - W / 2, b = a + W;
        const double c = pC.sum (a, b);
        if (c < 1.0) continue;
        const float local = msToLufs (pS.sum (a, b) / c);
        g[(size_t) i] = std::clamp (target - local, -range, range);
        has[(size_t) i] = 1;
    }

    // pausas: segura o ganho vizinho, mas nunca levanta abaixo do noisefloor
    {
        std::vector<float> prev ((size_t) N, 0.0f), next ((size_t) N, 0.0f);
        float h = 0.0f; bool any = false;
        for (int i = 0; i < N; ++i) { if (has[(size_t) i]) { h = g[(size_t) i]; any = true; } prev[(size_t) i] = any ? h : 0.0f; }
        h = 0.0f; any = false;
        for (int i = N - 1; i >= 0; --i) { if (has[(size_t) i]) { h = g[(size_t) i]; any = true; } next[(size_t) i] = any ? h : 0.0f; }
        for (int i = 0; i < N; ++i)
            if (! has[(size_t) i])
                g[(size_t) i] = std::min (0.0f, std::min (prev[(size_t) i], next[(size_t) i]));
    }

    // ---------------------------------------------------------------- Smoothing
    const double tau = (0.015 + 0.25 * st.smoothing) * std::sqrt (std::max (0.1, st.speedMs * 0.001) / 0.75);
    zeroPhaseOnePole (g, tau);

    // teto nas pausas: depois da suavizacao, o ganho nao pode passar de 0 dB fora da voz
    {
        std::vector<float> cap ((size_t) N);
        const int dil = 4;   // 40 ms
        for (int i = 0; i < N; ++i)
        {
            bool v = false;
            for (int k = std::max (0, i - dil); k <= std::min (N - 1, i + dil) && ! v; ++k) v = rawActive[(size_t) k] != 0;
            cap[(size_t) i] = v ? range : 0.0f;
        }
        // dentro das frases (buracos curtos ja preenchidos) o teto nao atua
        for (int i = 0; i < N; ++i) if (active[(size_t) i] && ! rawActive[(size_t) i])
        {
            // procura se e buraco interno (ha voz dos dois lados a menos de 250 ms)
            int l = i, r = i;
            while (l > 0 && ! rawActive[(size_t) l] && i - l < 25) --l;
            while (r < N - 1 && ! rawActive[(size_t) r] && r - i < 25) ++r;
            if (rawActive[(size_t) l] && rawActive[(size_t) r] && (r - l) < 25) cap[(size_t) i] = range;
        }
        zeroPhaseOnePole (cap, 0.030);
        for (int i = 0; i < N; ++i) g[(size_t) i] = std::min (g[(size_t) i], std::max (0.0f, cap[(size_t) i]));
    }

    // ---------------------------------------------------------------- Catch Peaks
    if (st.catchPeaks > 0.001f)
    {
        const float margin = 9.0f - 6.0f * st.catchPeaks;
        const float amount = 0.4f + 0.6f * st.catchPeaks;
        std::vector<float> r ((size_t) N, 0.0f);
        for (int i = 0; i < N; ++i)
        {
            if (! active[(size_t) i]) continue;
            const float excess = (fast[(size_t) i] + g[(size_t) i]) - (target + margin);
            if (excess > 0.0f) r[(size_t) i] = -excess * amount;
        }
        std::vector<float> e ((size_t) N, 0.0f);   // erosao +-40 ms (ataque antecipado)
        const int er = 4;
        for (int i = 0; i < N; ++i)
        {
            float m = 0.0f;
            for (int k = std::max (0, i - er); k <= std::min (N - 1, i + er); ++k) m = std::min (m, r[(size_t) k]);
            e[(size_t) i] = m;
        }
        zeroPhaseOnePole (e, 0.020);
        for (int i = 0; i < N; ++i) g[(size_t) i] += e[(size_t) i];
    }

    // ---------------------------------------------------------------- correcao final de nivel
    float offset = 0.0f;
    if (st.targetMode == TargetMode::lufsIMatch || st.targetMode == TargetMode::lufsICustom)
    {
        const float outI = integratedLoudness (ms, g.data());
        if (outI > -69.0f) offset = target - outI;
    }
    else
    {
        const auto shortOut = windowedLoudness (ms, g.data(), 300);
        std::vector<float> stAct;
        for (int i = 0; i < N; i += 10) if (active[(size_t) i] && shortOut[(size_t) i] > -70.0f) stAct.push_back (shortOut[(size_t) i]);
        const float p = percentile (stAct, 0.95f);
        if (p > -69.0f) offset = target - p;
    }
    offset = std::clamp (offset, -12.0f, 12.0f);

    for (int i = 0; i < N; ++i)
    {
        const float autoG = std::clamp (g[(size_t) i] + offset, -36.0f, 24.0f);
        curve->autoDb[(size_t) i] = autoG;
        float total = autoG;
        if (edits != nullptr && i < (int) edits->size()) total += (*edits)[(size_t) i];
        total = std::clamp (total, -48.0f, 24.0f);
        curve->gainDb[(size_t) i]  = total;
        curve->gainLin[(size_t) i] = dbToLin (total);
    }

    curve->outputLufsI = integratedLoudness (ms, curve->gainDb.data());
    {
        const auto shortOut = windowedLoudness (ms, curve->gainDb.data(), 300);
        std::vector<float> stAct;
        for (int i = 0; i < N; i += 10) if (active[(size_t) i] && shortOut[(size_t) i] > -70.0f) stAct.push_back (shortOut[(size_t) i]);
        curve->outputShortMax = percentile (stAct, 0.98f);
    }
    return curve;
}

//==============================================================================
void RealtimeLeveler::prepare (double sampleRate, int numChannels)
{
    fs = sampleRate;
    hop = std::max (1, (int) std::lround (fs * kHopSeconds));
    numCh = std::max (1, numChannels);
    lookaheadFrames = (int) std::lround (lookaheadSeconds / kHopSeconds);
    filters.assign ((size_t) numCh, {});
    for (auto& f : filters) f.prepare (fs);
    delaySize = lookaheadFrames * hop;
    delay.assign ((size_t) numCh, std::vector<float> ((size_t) delaySize, 0.0f));
    reset();
}

void RealtimeLeveler::reset()
{
    for (auto& f : filters) f.reset();
    for (auto& d : delay) std::fill (d.begin(), d.end(), 0.0f);
    delayPos = 0;
    std::fill (std::begin (frameMs), std::end (frameMs), 0.0f);
    std::fill (std::begin (frameFast), std::end (frameFast), 0.0f);
    std::fill (std::begin (frameActive), std::end (frameActive), false);
    frameIndex = 0; hangover = 0; accMs = 0.0; inFrame = 0;
    runIntegratedMs = 0.0; runWeight = 0.0;
    levelGainDb = 0.0f; targetGainDb = 0.0f;
    rampFrom = rampTo = 1.0f; rampPos = 0;
    currentGainDb = 0.0f;
}

void RealtimeLeveler::endOfFrame()
{
    const int n = frameIndex;
    const float msNow = (float) (accMs / hop);
    frameMs[n % frameRing] = msNow;
    accMs = 0.0;

    // loudness rapida (50 ms)
    double s = 0.0;
    for (int k = 0; k < 5; ++k) s += frameMs[(n - k + frameRing * 4) % frameRing];
    const float fastL = msToLufs (s / 5.0);
    frameFast[n % frameRing] = fastL;

    // integrada "corrente" (esquecimento ~10 s) - usada como alvo nos modos Match e no piso Auto
    if (fastL > -60.0f)
    {
        const double a = std::exp (-kHopSeconds / 10.0);
        if (runWeight < 1.0e-9) runIntegratedMs = msNow;
        runIntegratedMs = a * runIntegratedMs + (1.0 - a) * msNow;
        runWeight = 1.0;
    }
    const float runI = runWeight > 0.0 ? msToLufs (runIntegratedMs) : -40.0f;

    const float floorDb = settings.floorAuto ? std::max (-65.0f, runI - 25.0f) : settings.floorDb;
    if (fastL > floorDb) hangover = 15; else if (hangover > 0) --hangover;
    const bool active = hangover > 0;
    frameActive[n % frameRing] = active;

    // janela de loudness (Speed), so quadros com voz
    const int W = std::clamp ((int) std::lround (settings.speedMs * 0.001 / kHopSeconds), 3, frameRing - 64);
    double sa = 0.0; int ca = 0;
    for (int k = 0; k < W && k <= n; ++k)
    {
        const int idx = (n - k) % frameRing;
        if (frameActive[idx]) { sa += frameMs[idx]; ++ca; }
    }

    double ss = 0.0; int cs = 0;   // short-term 3 s para o medidor
    for (int k = 0; k < 300 && k <= n; ++k) { ss += frameMs[(n - k) % frameRing]; ++cs; }
    inShortLufs = cs > 0 ? msToLufs (ss / cs) : kMinDb;

    float target = settings.targetLufs;
    if (settings.targetMode == TargetMode::lufsIMatch || settings.targetMode == TargetMode::lufsSMatch)
        target = runI;
    lastTarget = target;

    float desired;
    if (active && ca > 0)
        desired = std::clamp (target - msToLufs (sa / ca), -settings.rangeDb, settings.rangeDb);
    else
        desired = std::min (levelGainDb, 0.0f);

    const double tau = (0.015 + 0.25 * settings.smoothing) * std::sqrt (std::max (0.1, settings.speedMs * 0.001) / 0.75);
    const float a = (float) std::exp (-kHopSeconds / tau);
    levelGainDb = a * levelGainDb + (1.0f - a) * desired;

    // Catch Peaks: olha os quadros que vao sair agora (+-40 ms) - estao 300 ms "atras" do mais novo
    float red = 0.0f;
    if (settings.catchPeaks > 0.001f)
    {
        const float margin = 9.0f - 6.0f * settings.catchPeaks;
        const float amount = 0.4f + 0.6f * settings.catchPeaks;
        const int o = n - lookaheadFrames;
        for (int k = o - 4; k <= o + 4; ++k)
        {
            if (k < 0) continue;
            const int idx = k % frameRing;
            if (! frameActive[idx]) continue;
            const float excess = frameFast[idx] + levelGainDb - (target + margin);
            if (excess > 0.0f) red = std::min (red, -excess * amount);
        }
    }

    targetGainDb = levelGainDb + red;
    history[historyWrite]   = targetGainDb;
    historyIn[historyWrite] = fastL;
    historyWrite = (historyWrite + 1) % historySize;

    rampFrom = rampTo;
    rampTo = dbToLin (targetGainDb);
    rampPos = 0;
    ++frameIndex;
}

void RealtimeLeveler::process (float* const* ch, int numChannels, int numSamples, bool bypassed)
{
    const int nch = std::min (numChannels, numCh);
    for (int i = 0; i < numSamples; ++i)
    {
        for (int c = 0; c < nch; ++c)
        {
            const float x = ch[c][i];
            const float y = filters[(size_t) c].process (x);
            accMs += (double) y * (double) y;
        }

        const float gl = rampFrom + (rampTo - rampFrom) * ((float) rampPos / (float) hop);
        if (rampPos < hop) ++rampPos;

        for (int c = 0; c < nch; ++c)
        {
            auto& d = delay[(size_t) c];
            const float delayed = d[(size_t) delayPos];
            d[(size_t) delayPos] = ch[c][i];
            ch[c][i] = bypassed ? delayed : delayed * gl;
        }
        for (int c = nch; c < numChannels; ++c) ch[c][i] = 0.0f;
        if (++delayPos >= delaySize) delayPos = 0;

        if (++inFrame >= hop) { inFrame = 0; endOfFrame(); }
    }
    currentGainDb = 20.0f * std::log10 (std::max (1.0e-6f, rampTo));
}

} // namespace vx
