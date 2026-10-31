#pragma once
/*
    VX_Leveler SILVER - nucleo DSP (C++ puro, sem JUCE)

    1) Analyzer      : le o audio inteiro (ARA) e gera quadros de 10 ms com
                       energia K-weighted (BS.1770) e pico.
    2) computeGainCurve : a partir da analise + ajustes (Speed, Smoothing,
                       Noisefloor, Target, Catch Peaks, Range) + edicoes
                       manuais, gera a curva de ganho (dB) por quadro.
                       Como conhece o take inteiro, o nivelamento e
                       "nao causal" (olha antes e depois de cada frase).
    3) RealtimeLeveler : modo de reserva para hosts sem ARA - "rider" com
                       lookahead de 300 ms.
*/

#include <vector>
#include <memory>
#include <cstdint>
#include <cmath>

namespace vx
{
constexpr double kHopSeconds = 0.010;   // resolucao da analise / curva
constexpr float  kMinDb      = -100.0f;

inline float msToLufs (double ms) noexcept   { return (float) (-0.691 + 10.0 * std::log10 (ms + 1.0e-12)); }
inline float dbToLin  (float db) noexcept     { return std::pow (10.0f, db * 0.05f); }

//==============================================================================
/** Filtro K (BS.1770): shelf de agudos + passa-altas RLB. */
struct KWeighting
{
    void prepare (double sampleRate) noexcept;
    void reset() noexcept { for (auto& s : z) s = 0.0; }
    inline float process (float x) noexcept
    {
        // estagio 1
        double y1 = b1[0] * x + z[0];
        z[0] = b1[1] * x - a1[1] * y1 + z[1];
        z[1] = b1[2] * x - a1[2] * y1;
        // estagio 2
        double y2 = b2[0] * y1 + z[2];
        z[2] = b2[1] * y1 - a2[1] * y2 + z[3];
        z[3] = b2[2] * y1 - a2[2] * y2;
        return (float) y2;
    }

    double b1[3] {}, a1[3] {}, b2[3] {}, a2[3] {};
    double z[4] {};
};

//==============================================================================
struct Analysis
{
    double sampleRate = 48000.0;
    int    hopSamples = 480;
    int64_t numSamples = 0;
    int    numChannels = 1;
    std::vector<float> ms;     // energia K-weighted (soma dos canais) por quadro
    std::vector<float> peak;   // pico absoluto por quadro (linear)

    int numFrames() const noexcept { return (int) ms.size(); }
};

/** Analise incremental: chame process() em blocos e finish() no final. */
class Analyzer
{
public:
    Analyzer (double sampleRate, int numChannels, int64_t totalSamples);
    void process (const float* const* channels, int numChannels, int numSamples);
    std::shared_ptr<Analysis> finish();

private:
    std::shared_ptr<Analysis> result;
    std::vector<KWeighting> filters;
    double accMs = 0.0;
    float  accPeak = 0.0f;
    int    inFrame = 0;
};

//==============================================================================
enum class TargetMode : int { lufsIMatch = 0, lufsICustom, lufsSMatch, lufsSCustom };

struct Settings
{
    float speedMs     = 750.0f;   // 100 .. 5000
    float smoothing   = 0.5f;     // 0 .. 1
    float catchPeaks  = 0.5f;     // 0 .. 1
    float rangeDb     = 18.0f;    // limite de boost/corte automatico
    bool  floorAuto   = true;
    float floorDb     = -40.0f;   // Noisefloor custom
    TargetMode targetMode = TargetMode::lufsIMatch;
    float targetLufs  = -23.0f;   // -32 .. -10

    static bool same (float a, float b) noexcept { return std::abs (a - b) < 1.0e-6f; }
    bool operator== (const Settings& o) const noexcept
    {
        return same (speedMs, o.speedMs) && same (smoothing, o.smoothing) && same (catchPeaks, o.catchPeaks)
            && same (rangeDb, o.rangeDb) && floorAuto == o.floorAuto && same (floorDb, o.floorDb)
            && targetMode == o.targetMode && same (targetLufs, o.targetLufs);
    }
    bool operator!= (const Settings& o) const noexcept { return ! (*this == o); }
};

//==============================================================================
struct GainCurve
{
    double sampleRate = 48000.0;
    int    hopSamples = 480;
    std::vector<float> autoDb;    // curva automatica (sem edicoes manuais)
    std::vector<float> gainDb;    // curva final (automatica + manual)
    std::vector<float> gainLin;   // idem, linear (usada no audio)

    float inputLufsI   = kMinDb;
    float outputLufsI  = kMinDb;
    float inputShortMax  = kMinDb;
    float outputShortMax = kMinDb;
    float floorDb      = -40.0f;
    float targetDb     = -23.0f;

    int numFrames() const noexcept { return (int) gainDb.size(); }

    /** Ganho linear interpolado na posicao (em amostras da fonte). */
    inline float gainAtSample (double samplePos) const noexcept
    {
        const int n = (int) gainLin.size();
        if (n == 0) return 1.0f;
        const double p = samplePos / (double) hopSamples - 0.5;
        if (p <= 0.0)               return gainLin.front();
        const int i = (int) p;
        if (i >= n - 1)             return gainLin.back();
        const float f = (float) (p - (double) i);
        return gainLin[(size_t) i] + f * (gainLin[(size_t) i + 1] - gainLin[(size_t) i]);
    }

    inline float gainDbAtTime (double seconds) const noexcept
    {
        const int n = (int) gainDb.size();
        if (n == 0) return 0.0f;
        int i = (int) (seconds * sampleRate / hopSamples);
        i = i < 0 ? 0 : (i >= n ? n - 1 : i);
        return gainDb[(size_t) i];
    }
};

std::shared_ptr<GainCurve> computeGainCurve (const Analysis& analysis,
                                             const Settings& settings,
                                             const std::vector<float>* manualEditsDb);

/** Loudness integrada BS.1770 (gates -70 / -10) a partir dos quadros, com ganho opcional por quadro. */
float integratedLoudness (const std::vector<float>& ms, const float* gainDb);

//==============================================================================
/** Modo tempo real (sem ARA): lookahead de 300 ms. */
class RealtimeLeveler
{
public:
    static constexpr double lookaheadSeconds = 0.300;

    void prepare (double sampleRate, int numChannels);
    void reset();
    void setSettings (const Settings& s) noexcept { settings = s; }
    void process (float* const* channels, int numChannels, int numSamples, bool bypassed);

    int   getLatencySamples() const noexcept { return lookaheadFrames * hop; }
    float getCurrentGainDb() const noexcept  { return currentGainDb; }
    float getInputShortLufs() const noexcept { return inShortLufs; }
    float getTargetDb() const noexcept       { return lastTarget; }

    /** historico de ganho (1 valor a cada 10 ms) para o display */
    static constexpr int historySize = 1000;
    float history[historySize] {};
    float historyIn[historySize] {};
    int   historyWrite = 0;

private:
    void endOfFrame();

    Settings settings;
    double fs = 48000.0;
    int hop = 480, numCh = 2, lookaheadFrames = 30;
    std::vector<KWeighting> filters;
    std::vector<std::vector<float>> delay;
    int delaySize = 0, delayPos = 0;

    static constexpr int frameRing = 1024;
    float frameMs[frameRing] {};
    float frameFast[frameRing] {};
    bool  frameActive[frameRing] {};
    int   frameIndex = 0;
    int   hangover = 0;

    double accMs = 0.0;
    int inFrame = 0;

    double runIntegratedMs = 0.0;
    double runWeight = 0.0;

    float levelGainDb = 0.0f;     // rider (suavizado)
    float targetGainDb = 0.0f;    // ganho final do quadro atual
    float rampFrom = 1.0f, rampTo = 1.0f;
    int   rampPos = 0;
    float currentGainDb = 0.0f;
    float inShortLufs = kMinDb;
    float lastTarget = -23.0f;
};

} // namespace vx
