#include "LevelerDSP.h"
#include <algorithm>
#include <numeric>

namespace vx
{
static constexpr double kPi = 3.14159265358979323846;

//==============================================================================
//  Biquad (RBJ cookbook)
//==============================================================================
static void normalise (Biquad& f, double b0, double b1, double b2, double a0, double a1, double a2) noexcept
{
    f.b0 = b0 / a0; f.b1 = b1 / a0; f.b2 = b2 / a0; f.a1 = a1 / a0; f.a2 = a2 / a0;
}

void Biquad::setLowpass (double fs, double f, double q) noexcept
{
    const double w = 2.0 * kPi * std::min (f, fs * 0.45) / fs, c = std::cos (w), al = std::sin (w) / (2.0 * q);
    normalise (*this, (1 - c) / 2, 1 - c, (1 - c) / 2, 1 + al, -2 * c, 1 - al);
}

void Biquad::setHighpass (double fs, double f, double q) noexcept
{
    const double w = 2.0 * kPi * std::min (f, fs * 0.45) / fs, c = std::cos (w), al = std::sin (w) / (2.0 * q);
    normalise (*this, (1 + c) / 2, -(1 + c), (1 + c) / 2, 1 + al, -2 * c, 1 - al);
}

void Biquad::setBandpass (double fs, double f, double q) noexcept
{
    const double w = 2.0 * kPi * std::min (f, fs * 0.45) / fs, c = std::cos (w), al = std::sin (w) / (2.0 * q);
    normalise (*this, al, 0.0, -al, 1 + al, -2 * c, 1 - al);
}

void Biquad::setPeaking (double fs, double f, double q, double gainDb) noexcept
{
    const double A = std::pow (10.0, gainDb / 40.0);
    const double w = 2.0 * kPi * std::min (f, fs * 0.45) / fs, c = std::cos (w), al = std::sin (w) / (2.0 * q);
    normalise (*this, 1 + al * A, -2 * c, 1 - al * A, 1 + al / A, -2 * c, 1 - al / A);
}

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
//  Analyzer
//==============================================================================
Analyzer::Analyzer (double sampleRate, int numChannels, int64_t totalSamples)
{
    result = std::make_shared<Analysis>();
    result->sampleRate  = sampleRate;
    result->hopSamples  = std::max (1, (int) std::lround (sampleRate * kHopSeconds));
    result->numSamples  = totalSamples;
    result->numChannels = std::max (1, numChannels);
    const auto frames = (size_t) (totalSamples / result->hopSamples + 2);
    for (auto* v : { &result->ms, &result->peak, &result->voicing, &result->zcr, &result->totMs, &result->hfMs, &result->sibFreq })
        v->reserve (frames);

    filters.resize ((size_t) result->numChannels);
    for (auto& f : filters) f.prepare (sampleRate);

    const double fs = sampleRate;
    hp80.setHighpass (fs, 80.0, 0.707);
    hfA.setHighpass (fs, 4500.0, 0.541);
    hfB.setHighpass (fs, 4500.0, 1.307);

    const double centres[4] = { 5000.0, 7000.0, 9500.0, 12500.0 };
    numBands = 0;
    for (double c : centres)
        if (c < fs * 0.42)
        {
            bandFreq[numBands] = c;
            bands[numBands].setBandpass (fs, c, 1.6);
            ++numBands;
        }

    decimation = std::max (1, (int) std::lround (fs / 6000.0));
    fsDecim = fs / decimation;
    decA.setLowpass (fs, std::min (2200.0, fsDecim * 0.4), 0.541);
    decB.setLowpass (fs, std::min (2200.0, fsDecim * 0.4), 1.307);
}

void Analyzer::process (const float* const* channels, int numChannels, int numSamples)
{
    const int nch = std::min (numChannels, (int) filters.size());
    const int hop = result->hopSamples;
    const float invCh = 1.0f / (float) std::max (1, nch);

    for (int i = 0; i < numSamples; ++i)
    {
        float mono = 0.0f;
        for (int c = 0; c < nch; ++c)
        {
            const float x = channels[c][i];
            const float y = filters[(size_t) c].process (x);
            accMs += (double) y * (double) y;
            accPeak = std::max (accPeak, std::abs (x));
            mono += x;
        }
        mono *= invCh;

        // caracteristicas da voz
        const double h = hp80.process (mono);
        accTot += h * h;
        const bool pos = h > 1.0e-5;
        const bool neg = h < -1.0e-5;
        if ((pos && ! lastPositive) || (neg && lastPositive)) { if (pos || neg) ++zc; }
        if (pos) lastPositive = true; else if (neg) lastPositive = false;

        const double hf = hfB.process (hfA.process (h));
        accHf += hf * hf;
        for (int b = 0; b < numBands; ++b)
        {
            const double v = bands[b].process (h);
            accBand[b] += v * v;
        }

        const double d = decB.process (decA.process (h));
        if (++decimCounter >= decimation)
        {
            decimCounter = 0;
            ring[ringPos] = (float) d;
            ringPos = (ringPos + 1) % ringSize;
            if (ringFilled < ringSize) ++ringFilled;
        }

        if (++inFrame == hop)
            endFrame (hop);
    }
}

void Analyzer::endFrame (int n)
{
    auto& r = *result;
    r.ms.push_back ((float) (accMs / n));
    r.peak.push_back (accPeak);
    r.totMs.push_back ((float) (accTot / n));
    r.hfMs.push_back ((float) (accHf / n));
    r.zcr.push_back ((float) ((double) zc / ((double) n / r.sampleRate)));

    int best = 0;
    for (int b = 1; b < numBands; ++b) if (accBand[b] > accBand[best]) best = b;
    r.sibFreq.push_back (numBands > 0 ? (float) bandFreq[best] : 6000.0f);

    // voicing: autocorrelacao normalizada (32 ms, 70..450 Hz) no sinal decimado
    float voicing = 0.0f;
    const int W = (int) std::lround (0.032 * fsDecim);
    const int minLag = std::max (2, (int) std::floor (fsDecim / 450.0));
    const int maxLag = (int) std::ceil (fsDecim / 70.0);
    if (ringFilled >= W + maxLag + 1)
    {
        auto at = [this] (int back) { return (double) ring[(ringPos - 1 - back + ringSize * 4) % ringSize]; };
        double e0 = 0.0;
        for (int k = 0; k < W; ++k) { const double v = at (k); e0 += v * v; }
        if (e0 > 1.0e-10)
        {
            double el = 0.0;
            for (int k = 0; k < W; ++k) { const double v = at (k + minLag); el += v * v; }
            double best = 0.0;
            for (int lag = minLag; lag <= maxLag; ++lag)
            {
                double xc = 0.0;
                for (int k = 0; k < W; ++k) xc += at (k) * at (k + lag);
                const double nrm = xc / std::sqrt (e0 * el + 1.0e-20);
                best = std::max (best, nrm);
                // energia da janela atrasada para o proximo lag (desliza 1 amostra)
                const double outV = at (lag), inV = at (lag + W);
                el += inV * inV - outV * outV;
                if (el < 1.0e-20) el = 1.0e-20;
            }
            voicing = (float) std::clamp (best, 0.0, 1.0);
        }
    }
    r.voicing.push_back (voicing);

    accMs = 0.0; accPeak = 0.0f; accTot = 0.0; accHf = 0.0; zc = 0; inFrame = 0;
    for (auto& b : accBand) b = 0.0;
}

std::shared_ptr<Analysis> Analyzer::finish()
{
    if (inFrame > 0)
        endFrame (inFrame);
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

    struct Seg { int a, b; };   // [a, b)

    /** agrupa quadros marcados em segmentos (fecha buracos <= gap) e filtra pela duracao */
    std::vector<Seg> segments (const std::vector<uint8_t>& mask, int gap, int minLen, int maxLen)
    {
        std::vector<Seg> out;
        const int N = (int) mask.size();
        int i = 0;
        while (i < N)
        {
            if (! mask[(size_t) i]) { ++i; continue; }
            int a = i, b = i;
            int j = i;
            while (j < N)
            {
                if (mask[(size_t) j]) { b = j + 1; ++j; continue; }
                int k = j; while (k < N && ! mask[(size_t) k] && k - j <= gap) ++k;
                if (k < N && mask[(size_t) k] && k - j <= gap) { j = k; continue; }
                break;
            }
            if (b - a >= minLen && b - a <= maxLen) out.push_back ({ a, b });
            i = std::max (b, i + 1);
        }
        return out;
    }

    /** rampa cosseno 0..1 */
    inline float ramp (float x) noexcept
    {
        x = std::clamp (x, 0.0f, 1.0f);
        return 0.5f - 0.5f * std::cos ((float) kPi * x);
    }

    /** peso dentro de [a,b) com rampas de t quadros nas bordas (por dentro) */
    inline float insideWeight (int f, int a, int b, float t) noexcept
    {
        if (f < a || f >= b) return 0.0f;
        if (t <= 0.5f) return 1.0f;
        const float len = (float) (b - a);
        const float tt = std::min (t, len * 0.5f);
        const float w1 = ramp (((float) (f - a) + 0.5f) / tt);
        const float w2 = ramp (((float) (b - f) - 0.5f) / tt);
        return std::min (w1, w2);
    }

    bool isDisabled (const std::vector<RegionKey>* dis, RegionType t, int start)
    {
        if (dis == nullptr) return false;
        for (auto& k : *dis)
            if (k.type == (int) t && std::abs (k.startFrame - start) <= 6) return true;
        return false;
    }
}

float integratedLoudness (const std::vector<float>& ms, const float* gainDb)
{
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
//  Curva de ganho: GATE -> DYNAMICS -> BREATH -> SIBILANCE
//==============================================================================
std::shared_ptr<GainCurve> computeGainCurve (const Analysis& an, const Settings& st,
                                             const std::vector<float>* edits,
                                             const std::vector<RegionKey>* disabled)
{
    auto curve = std::make_shared<GainCurve>();
    curve->sampleRate = an.sampleRate;
    curve->hopSamples = an.hopSamples;

    const int N = an.numFrames();
    curve->autoDb.assign ((size_t) N, 0.0f);
    curve->gainDb.assign ((size_t) N, 0.0f);
    curve->gainLin.assign ((size_t) N, 1.0f);
    curve->sibCutDb.assign ((size_t) N, 0.0f);
    curve->sibFreq.assign ((size_t) N, 6000.0f);
    if (N == 0) return curve;

    const auto& ms = an.ms;
    const bool hasFeatures = (int) an.voicing.size() == N && (int) an.zcr.size() == N
                          && (int) an.hfMs.size() == N && (int) an.totMs.size() == N;

    const float inI = integratedLoudness (ms, nullptr);
    curve->inputLufsI = inI;
    if (inI <= -69.0f)
    {
        curve->outputLufsI = inI;
        return curve;
    }

    const auto fast    = windowedLoudness (ms, nullptr, 5);     // 50 ms
    const auto block   = windowedLoudness (ms, nullptr, 40);    // 400 ms
    const auto shortIn = windowedLoudness (ms, nullptr, 300);   // 3 s

    // ---------------------------------------------------------------- pisos
    // ruido de fundo: 5o percentil da loudness rapida (pausas de 50 ms ja bastam)
    float noiseEst;
    {
        std::vector<float> fr;
        for (int i = 0; i < N; i += 2) if (fast[(size_t) i] > -95.0f) fr.push_back (fast[(size_t) i]);
        noiseEst = fr.empty() ? -90.0f : percentile (fr, 0.05f);
    }

    std::vector<float> hfRatio ((size_t) N, 0.0f);
    float voiceMedian = inI, voiceLoud = inI;
    bool  enoughVoice = false;
    if (hasFeatures)
    {
        std::vector<float> v;
        for (int i = 0; i < N; ++i)
        {
            hfRatio[(size_t) i] = an.hfMs[(size_t) i] / (an.totMs[(size_t) i] + 1.0e-12f);
            if ((i & 1) == 0 && an.voicing[(size_t) i] > 0.55f && fast[(size_t) i] > noiseEst + 10.0f)
                v.push_back (fast[(size_t) i]);
        }
        if (v.size() > 20) { voiceMedian = percentile (v, 0.5f); voiceLoud = percentile (v, 0.9f); enoughVoice = true; }
    }

    float floorDb = st.floorDb;
    if (st.floorAuto)
    {
        floorDb = std::max (noiseEst + 8.0f, inI - 30.0f);
        floorDb = std::min (floorDb, std::min (inI - 12.0f, (enoughVoice ? voiceMedian : inI) - 18.0f));
        floorDb = std::max (floorDb, -75.0f);
    }
    curve->floorDb = floorDb;

    // ---------------------------------------------------------------- classificacao dos quadros
    std::vector<uint8_t> voiced ((size_t) N, 0), sibCand ((size_t) N, 0), breathCand ((size_t) N, 0);
    for (int i = 0; i < N; ++i)
        voiced[(size_t) i] = hasFeatures ? (an.voicing[(size_t) i] > 0.55f && fast[(size_t) i] > floorDb)
                                         : (fast[(size_t) i] > floorDb);

    std::vector<Seg> sibSegs, breathSegs;
    std::vector<uint8_t> breathMask ((size_t) N, 0), sibMask ((size_t) N, 0);
    if (hasFeatures)
    {
        for (int i = 0; i < N; ++i)
            sibCand[(size_t) i] = fast[(size_t) i] > floorDb - 6.0f && hfRatio[(size_t) i] > 0.45f
                                  && an.voicing[(size_t) i] < 0.5f;
        sibSegs = segments (sibCand, 2, 2, 45);
        for (auto& s : sibSegs) for (int f = s.a; f < s.b; ++f) sibMask[(size_t) f] = 1;

        for (int i = 0; i < N; ++i)
            breathCand[(size_t) i] = ! sibMask[(size_t) i] && an.voicing[(size_t) i] < 0.4f
                                     && an.zcr[(size_t) i] > 1200.0f && hfRatio[(size_t) i] < 0.6f
                                     && fast[(size_t) i] > noiseEst + 6.0f && fast[(size_t) i] < voiceLoud - 3.0f;
        if (! enoughVoice) std::fill (breathCand.begin(), breathCand.end(), (uint8_t) 0);
        for (auto& s : segments (breathCand, 4, 15, 120))
        {
            int nv = 0; float mx = kMinDb;
            for (int f = s.a; f < s.b; ++f) { nv += voiced[(size_t) f]; mx = std::max (mx, fast[(size_t) f]); }
            if (nv * 5 > (s.b - s.a) || mx > voiceLoud - 3.0f) continue;
            breathSegs.push_back (s);
            for (int f = s.a; f < s.b; ++f) breathMask[(size_t) f] = 1;
        }
    }

    // ================================================================ GATE
    std::vector<float> gGate ((size_t) N, 0.0f);
    if (st.gateOn)
    {
        std::vector<uint8_t> voice ((size_t) N, 0);
        for (int i = 0; i < N; ++i)
            voice[(size_t) i] = st.gateMode == 0 ? (fast[(size_t) i] > floorDb && ! breathMask[(size_t) i])
                                                 : (fast[(size_t) i] > st.gateThreshDb);
        {   // protege finais de consoantes (30 ms)
            std::vector<uint8_t> d (voice);
            for (int i = 0; i < N; ++i)
                if (voice[(size_t) i])
                    for (int k = std::max (0, i - 3); k <= std::min (N - 1, i + 3); ++k) d[(size_t) k] = 1;
            voice.swap (d);
        }

        const int minLen = std::max (1, (int) std::lround (st.gateMinLenMs * 0.1));
        const float closeF = std::max (1.0f, st.gateCloseMs * 0.1f), openF = std::max (1.0f, st.gateOpenMs * 0.1f);
        const float rLin = st.gateReduction <= kInfDb + 0.01f ? 0.0f : dbToLin (st.gateReduction);

        int i = 0;
        while (i < N)
        {
            if (voice[(size_t) i]) { ++i; continue; }
            int j = i; while (j < N && ! voice[(size_t) j]) ++j;
            if (j - i >= minLen)
            {
                const bool off = isDisabled (disabled, RegionType::gate, i);
                curve->regions.push_back ({ RegionType::gate, i, j, st.gateReduction, 0.0f, ! off });
                ++curve->numGate;
                if (! off)
                {
                    const float len = (float) (j - i);
                    const float sc = std::min (1.0f, len / (closeF + openF));
                    const float c = closeF * sc, o = openF * sc;
                    for (int f = i; f < j; ++f)
                    {
                        const float wIn  = (i == 0) ? 1.0f : ramp (((float) (f - i) + 0.5f) / c);
                        const float wOut = (j == N) ? 1.0f : ramp (((float) (j - f) - 0.5f) / o);
                        const float w = std::min (wIn, wOut);
                        gGate[(size_t) f] = std::max (kInfDb, linToDb (1.0f + (rLin - 1.0f) * w));
                    }
                }
            }
            i = j;
        }
    }

    // ================================================================ DYNAMICS
    std::vector<float> g ((size_t) N, 0.0f);
    float target = st.targetLufs;
    {
        std::vector<float> stAct;
        for (int i = 0; i < N; i += 10) if (fast[(size_t) i] > floorDb && shortIn[(size_t) i] > -70.0f) stAct.push_back (shortIn[(size_t) i]);
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

    if (st.dynOn)
    {
        std::vector<uint8_t> active ((size_t) N, 0);
        for (int i = 0; i < N; ++i) active[(size_t) i] = fast[(size_t) i] > floorDb && ! breathMask[(size_t) i] ? 1 : 0;
        const std::vector<uint8_t> rawActive (active);

        {   // hangover 150 ms + fecha buracos < 250 ms dentro das frases
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
            for (int k = 0; k < N; ++k) if (breathMask[(size_t) k]) d[(size_t) k] = 0;   // respiros nao contam como voz
            active.swap (d);
        }

        const int W = std::max (3, (int) std::lround (st.speedMs * 0.001 / kHopSeconds));
        std::vector<float> msAct ((size_t) N), cnt ((size_t) N);
        for (int i = 0; i < N; ++i) { msAct[(size_t) i] = active[(size_t) i] ? ms[(size_t) i] : 0.0f; cnt[(size_t) i] = active[(size_t) i] ? 1.0f : 0.0f; }
        Prefix pS (msAct), pC (cnt);

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

        {   // pausas: segura o ganho vizinho, mas nunca levanta
            std::vector<float> prev ((size_t) N, 0.0f), next ((size_t) N, 0.0f);
            float h = 0.0f; bool any = false;
            for (int i = 0; i < N; ++i) { if (has[(size_t) i]) { h = g[(size_t) i]; any = true; } prev[(size_t) i] = any ? h : 0.0f; }
            h = 0.0f; any = false;
            for (int i = N - 1; i >= 0; --i) { if (has[(size_t) i]) { h = g[(size_t) i]; any = true; } next[(size_t) i] = any ? h : 0.0f; }
            for (int i = 0; i < N; ++i)
                if (! has[(size_t) i])
                    g[(size_t) i] = std::min (0.0f, std::min (prev[(size_t) i], next[(size_t) i]));
        }

        const double tau = (0.015 + 0.25 * st.smoothing) * std::sqrt (std::max (0.1, st.speedMs * 0.001) / 0.75);
        zeroPhaseOnePole (g, tau);

        {   // teto nas pausas (e respiros): nunca levanta fora da voz
            std::vector<float> cap ((size_t) N);
            const int dil = 4;
            for (int i = 0; i < N; ++i)
            {
                bool v = false;
                for (int k = std::max (0, i - dil); k <= std::min (N - 1, i + dil) && ! v; ++k)
                    v = rawActive[(size_t) k] != 0;
                cap[(size_t) i] = v ? range : 0.0f;
            }
            for (int i = 0; i < N; ++i) if (active[(size_t) i] && ! rawActive[(size_t) i] && ! breathMask[(size_t) i])
            {
                int l = i, r = i;
                while (l > 0 && ! rawActive[(size_t) l] && i - l < 25) --l;
                while (r < N - 1 && ! rawActive[(size_t) r] && r - i < 25) ++r;
                if (rawActive[(size_t) l] && rawActive[(size_t) r] && (r - l) < 25) cap[(size_t) i] = range;
            }
            zeroPhaseOnePole (cap, 0.030);
            for (int i = 0; i < N; ++i) g[(size_t) i] = std::min (g[(size_t) i], std::max (0.0f, cap[(size_t) i]));
        }

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
            std::vector<float> e ((size_t) N, 0.0f);
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
        for (auto& v : g) v += offset;
    }

    // ================================================================ BREATH
    std::vector<float> gBreath ((size_t) N, 0.0f);
    {
        // nivel da voz depois do Dynamics
        std::vector<float> v;
        for (int i = 0; i < N; i += 2) if (voiced[(size_t) i]) v.push_back (fast[(size_t) i] + g[(size_t) i]);
        const float postVoice = v.size() > 20 ? percentile (v, 0.5f) : voiceMedian;

        for (auto& s : breathSegs)
        {
            const bool off = isDisabled (disabled, RegionType::breath, s.a);
            float L = kMinDb;
            for (int f = s.a; f < s.b; ++f) L = std::max (L, fast[(size_t) f] + g[(size_t) f]);
            const float thr = st.breathAuto ? postVoice - 20.0f : st.breathThreshDb;
            float red = st.breathReduction <= kInfDb + 0.01f ? kInfDb : st.breathReduction + std::min (0.0f, thr - L);
            red = std::max (kInfDb, red);
            if (st.breathOn) { curve->regions.push_back ({ RegionType::breath, s.a, s.b, red, 0.0f, ! off }); ++curve->numBreath; }
            if (! st.breathOn || off) continue;

            const float rLin = red <= kInfDb + 0.01f ? 0.0f : dbToLin (red);
            const float t = std::max (1.0f, st.breathTransMs * 0.1f);
            for (int f = s.a; f < s.b; ++f)
            {
                const float w = insideWeight (f, s.a, s.b, t);
                gBreath[(size_t) f] = std::max (kInfDb, linToDb (1.0f + (rLin - 1.0f) * w));
            }
        }
    }

    // ================================================================ SIBILANCE
    std::vector<float> gSib ((size_t) N, 0.0f);
    if (st.sibOn && hasFeatures && ! sibSegs.empty())
    {
        // nivel local da voz (2 s) depois do Dynamics
        std::vector<float> vMs ((size_t) N), vCnt ((size_t) N);
        for (int i = 0; i < N; ++i)
        {
            const bool v = voiced[(size_t) i];
            vMs[(size_t) i]  = v ? ms[(size_t) i] * std::pow (10.0f, g[(size_t) i] * 0.1f) : 0.0f;
            vCnt[(size_t) i] = v ? 1.0f : 0.0f;
        }
        Prefix pM (vMs), pN (vCnt);
        const float sens = std::clamp (st.sibSpectral, 0.0f, 1.0f);
        const float t = st.sibTransMs * 0.1f;

        for (auto& s : sibSegs)
        {
            const bool off = isDisabled (disabled, RegionType::sibilance, s.a);
            float L = kMinDb;
            for (int f = s.a; f < s.b; ++f) L = std::max (L, fast[(size_t) f] + g[(size_t) f]);

            const int c = (s.a + s.b) / 2;
            const double cnt = pN.sum (c - 100, c + 100);
            const float localVoice = cnt > 5 ? msToLufs (pM.sum (c - 100, c + 100) / cnt) : voiceMedian + g[(size_t) c];
            const float thr = st.sibAuto ? localVoice - 3.0f : st.sibThreshDb;
            const float r = L - thr;
            if (r < 0.5f) continue;

            const float broadband = -r * (1.0f - 0.65f * sens);
            const float cut = std::min (20.0f, r * sens * 1.8f);

            // frequencia dominante (mais frequente no segmento)
            float freq = 6000.0f;
            {
                std::vector<float> fr;
                for (int f = s.a; f < s.b; ++f) fr.push_back (an.sibFreq[(size_t) f]);
                freq = percentile (fr, 0.5f);
            }

            curve->regions.push_back ({ RegionType::sibilance, s.a, s.b, broadband, cut, ! off });
            ++curve->numSib;
            if (off) continue;

            const int ext = (int) std::ceil (t);
            for (int f = std::max (0, s.a - ext); f < std::min (N, s.b + ext); ++f)
            {
                float w = 1.0f;
                if (f < s.a)       w = ramp (1.0f - (float) (s.a - f) / (t + 1.0f));
                else if (f >= s.b) w = ramp (1.0f - (float) (f - s.b + 1) / (t + 1.0f));
                gSib[(size_t) f] = std::min (gSib[(size_t) f], broadband * w);
                if (cut * w > curve->sibCutDb[(size_t) f])
                {
                    curve->sibCutDb[(size_t) f] = cut * w;
                    curve->sibFreq[(size_t) f]  = freq;
                }
            }
        }
        for (auto v : curve->sibCutDb) if (v > 0.05f) { curve->hasSpectral = true; break; }
    }

    // ================================================================ soma final
    for (int i = 0; i < N; ++i)
    {
        const float autoG = std::clamp (gGate[(size_t) i] + g[(size_t) i] + gBreath[(size_t) i] + gSib[(size_t) i], kInfDb, 24.0f);
        curve->autoDb[(size_t) i] = autoG;
        float total = autoG;
        if (edits != nullptr && i < (int) edits->size()) total += (*edits)[(size_t) i];
        total = std::clamp (total, kInfDb, 24.0f);
        curve->gainDb[(size_t) i]  = total;
        curve->gainLin[(size_t) i] = total <= kInfDb + 0.01f ? 0.0f : dbToLin (total);
    }

    curve->outputLufsI = integratedLoudness (ms, curve->gainDb.data());
    {
        const auto shortOut = windowedLoudness (ms, curve->gainDb.data(), 300);
        std::vector<float> stAct;
        for (int i = 0; i < N; i += 10) if (fast[(size_t) i] > floorDb && shortOut[(size_t) i] > -70.0f) stAct.push_back (shortOut[(size_t) i]);
        curve->outputShortMax = percentile (stAct, 0.98f);
    }
    return curve;
}

//==============================================================================
//  De-esser espectral
//==============================================================================
void SpectralDeEsser::prepare (double sampleRate, int)
{
    fs = sampleRate;
    reset();
}

void SpectralDeEsser::reset()
{
    for (auto& f : filt) f.reset();
    active = false;
}

void SpectralDeEsser::process (float* const* ch, int numChannels, int startInBuffer, int numSamples,
                               const GainCurve& curve, double srcStart, double srcStep)
{
    if (! curve.hasSpectral) { if (active) reset(); return; }
    const int nch = std::min (numChannels, maxCh);
    const int sub = 32;

    for (int pos = 0; pos < numSamples; pos += sub)
    {
        const int n = std::min (sub, numSamples - pos);
        float cut, freq;
        curve.sibAtSample (srcStart + (pos + n * 0.5) * srcStep, cut, freq);

        if (cut < 0.05f)
        {
            if (active) reset();
            continue;
        }

        active = true;
        Biquad proto;
        proto.setPeaking (fs, freq, 0.9, -cut);
        for (int c = 0; c < nch; ++c)
        {
            auto& f = filt[c];
            f.b0 = proto.b0; f.b1 = proto.b1; f.b2 = proto.b2; f.a1 = proto.a1; f.a2 = proto.a2;
            float* d = ch[c] + startInBuffer + pos;
            for (int i = 0; i < n; ++i) d[i] = (float) f.process (d[i]);
        }
    }
}

//==============================================================================
//  Tempo real
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
    if (! settings.dynOn) desired = 0.0f;
    levelGainDb = a * levelGainDb + (1.0f - a) * desired;

    // Catch Peaks: olha os quadros que vao sair agora (+-40 ms) - estao 300 ms "atras" do mais novo
    float red = 0.0f;
    if (settings.dynOn && settings.catchPeaks > 0.001f)
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
