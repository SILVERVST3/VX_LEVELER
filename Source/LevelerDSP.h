#pragma once
/*
    VX_Leveler SILVER - nucleo DSP (C++ puro, sem JUCE)

    1) Analyzer        : le o audio inteiro (ARA) e gera quadros de 10 ms com
                         energia K-weighted (BS.1770), pico, sonoridade (voicing),
                         taxa de cruzamentos por zero, energia de agudos e a
                         frequencia dominante dos sibilantes.
    2) computeGainCurve: a partir da analise + ajustes das secoes
                         GATE -> DYNAMICS -> BREATH -> SIBILANCE + edicoes manuais,
                         gera a curva de ganho (dB por quadro), a curva espectral
                         do de-esser e a lista de regioes detectadas.
    3) SpectralDeEsser : filtro peaking dinamico que aplica a parte espectral
                         do de-esser durante a reproducao / exportacao.
    4) RealtimeLeveler : modo de reserva para hosts sem ARA (so Dynamics).
*/

#include <vector>
#include <memory>
#include <cstdint>
#include <cmath>

namespace vx
{
constexpr double kHopSeconds = 0.010;   // resolucao da analise / curva
constexpr float  kMinDb      = -100.0f;
constexpr float  kInfDb      = -80.0f;  // "-inf" nos knobs de reducao

inline float msToLufs (double ms) noexcept   { return (float) (-0.691 + 10.0 * std::log10 (ms + 1.0e-12)); }
inline float dbToLin  (float db) noexcept     { return std::pow (10.0f, db * 0.05f); }
inline float linToDb  (float g) noexcept      { return 20.0f * std::log10 (g > 1.0e-9f ? g : 1.0e-9f); }

//==============================================================================
/** Biquad simples (TDF II) com projetos RBJ. */
struct Biquad
{
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    double z1 = 0, z2 = 0;

    void reset() noexcept { z1 = z2 = 0.0; }
    inline double process (double x) noexcept
    {
        const double y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
    void setLowpass  (double fs, double f, double q) noexcept;
    void setHighpass (double fs, double f, double q) noexcept;
    void setBandpass (double fs, double f, double q) noexcept;   // ganho 0 dB no centro
    void setPeaking  (double fs, double f, double q, double gainDb) noexcept;
};

/** Filtro K (BS.1770): shelf de agudos + passa-altas RLB. */
struct KWeighting
{
    void prepare (double sampleRate) noexcept;
    void reset() noexcept { for (auto& s : z) s = 0.0; }
    inline float process (float x) noexcept
    {
        double y1 = b1[0] * x + z[0];
        z[0] = b1[1] * x - a1[1] * y1 + z[1];
        z[1] = b1[2] * x - a1[2] * y1;
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
    std::vector<float> ms;       // energia K-weighted (soma dos canais) por quadro
    std::vector<float> peak;     // pico absoluto por quadro (linear)
    std::vector<float> voicing;  // 0..1 (autocorrelacao normalizada - voz "cantada/falada")
    std::vector<float> zcr;      // cruzamentos por zero por segundo
    std::vector<float> totMs;    // energia do sinal (HP 80 Hz)
    std::vector<float> hfMs;     // energia acima de 4,5 kHz
    std::vector<float> sibFreq;  // frequencia (Hz) da banda de agudos mais forte

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
    void endFrame (int samplesInFrame);

    std::shared_ptr<Analysis> result;
    std::vector<KWeighting> filters;
    double accMs = 0.0;
    float  accPeak = 0.0f;
    int    inFrame = 0;

    // caracteristicas da voz (mono)
    Biquad hp80, hfA, hfB, decA, decB;
    Biquad bands[4];
    double bandFreq[4] {};
    int    numBands = 0;
    double accTot = 0.0, accHf = 0.0, accBand[4] {};
    int    zc = 0;
    bool   lastPositive = false;

    int    decimation = 8, decimCounter = 0;
    double fsDecim = 6000.0;
    static constexpr int ringSize = 1024;
    float  ring[ringSize] {};
    int    ringPos = 0, ringFilled = 0;
};

//==============================================================================
enum class TargetMode : int { lufsIMatch = 0, lufsICustom, lufsSMatch, lufsSCustom };
enum class RegionType : int { gate = 0, breath = 1, sibilance = 2 };

struct RegionKey
{
    int type = 0;
    int startFrame = 0;
};

struct Region
{
    RegionType type = RegionType::gate;
    int   startFrame = 0, endFrame = 0;
    float depthDb = 0.0f;     // reducao aplicada (negativa)
    float extraDb = 0.0f;     // sibilancia: corte espectral
    bool  enabled = true;
};

struct Settings
{
    // ---- DYNAMICS
    bool  dynOn       = true;
    float speedMs     = 750.0f;   // 100 .. 5000
    float smoothing   = 0.5f;     // 0 .. 1
    float catchPeaks  = 0.5f;     // 0 .. 1
    float rangeDb     = 18.0f;    // limite de boost/corte automatico
    bool  floorAuto   = true;
    float floorDb     = -40.0f;   // Noisefloor custom
    TargetMode targetMode = TargetMode::lufsIMatch;
    float targetLufs  = -23.0f;   // -32 .. -10

    // ---- GATE
    bool  gateOn        = false;
    int   gateMode      = 0;        // 0 = Vocal Detection, 1 = Threshold Based
    float gateThreshDb  = -40.0f;
    float gateCloseMs   = 150.0f;
    float gateOpenMs    = 150.0f;
    float gateReduction = kInfDb;   // -80 = -inf
    float gateMinLenMs  = 250.0f;

    // ---- BREATH
    bool  breathOn        = true;
    float breathReduction = -3.0f;
    bool  breathAuto      = true;
    float breathThreshDb  = -40.0f;
    float breathTransMs   = 25.0f;

    // ---- SIBILANCE
    bool  sibOn        = true;
    bool  sibAuto      = true;
    float sibThreshDb  = -24.0f;
    float sibTransMs   = 20.0f;
    float sibSpectral  = 0.5f;      // 0 .. 1

    static bool same (float a, float b) noexcept { return std::abs (a - b) < 1.0e-6f; }
    bool operator== (const Settings& o) const noexcept
    {
        return dynOn == o.dynOn && same (speedMs, o.speedMs) && same (smoothing, o.smoothing)
            && same (catchPeaks, o.catchPeaks) && same (rangeDb, o.rangeDb) && floorAuto == o.floorAuto
            && same (floorDb, o.floorDb) && targetMode == o.targetMode && same (targetLufs, o.targetLufs)
            && gateOn == o.gateOn && gateMode == o.gateMode && same (gateThreshDb, o.gateThreshDb)
            && same (gateCloseMs, o.gateCloseMs) && same (gateOpenMs, o.gateOpenMs)
            && same (gateReduction, o.gateReduction) && same (gateMinLenMs, o.gateMinLenMs)
            && breathOn == o.breathOn && same (breathReduction, o.breathReduction) && breathAuto == o.breathAuto
            && same (breathThreshDb, o.breathThreshDb) && same (breathTransMs, o.breathTransMs)
            && sibOn == o.sibOn && sibAuto == o.sibAuto && same (sibThreshDb, o.sibThreshDb)
            && same (sibTransMs, o.sibTransMs) && same (sibSpectral, o.sibSpectral);
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
    std::vector<float> sibCutDb;  // corte espectral do de-esser (dB, positivo = corte)
    std::vector<float> sibFreq;   // frequencia do corte (Hz)
    bool hasSpectral = false;

    std::vector<Region> regions;
    int numGate = 0, numBreath = 0, numSib = 0;

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

    inline void sibAtSample (double samplePos, float& cutDb, float& freq) const noexcept
    {
        const int n = (int) sibCutDb.size();
        cutDb = 0.0f; freq = 6000.0f;
        if (n == 0) return;
        const double p = samplePos / (double) hopSamples - 0.5;
        int i = (int) std::floor (p);
        if (i < 0) i = 0;
        if (i >= n - 1) { cutDb = sibCutDb.back(); freq = sibFreq.back(); return; }
        const float f = (float) (p - (double) i);
        cutDb = sibCutDb[(size_t) i] + f * (sibCutDb[(size_t) i + 1] - sibCutDb[(size_t) i]);
        freq  = sibCutDb[(size_t) i] >= sibCutDb[(size_t) i + 1] ? sibFreq[(size_t) i] : sibFreq[(size_t) i + 1];
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
                                             const std::vector<float>* manualEditsDb,
                                             const std::vector<RegionKey>* disabledRegions = nullptr);

/** Loudness integrada BS.1770 (gates -70 / -10) a partir dos quadros, com ganho opcional por quadro. */
float integratedLoudness (const std::vector<float>& ms, const float* gainDb);

//==============================================================================
/** Parte espectral do de-esser: peaking dinamico seguindo a curva sibCutDb/sibFreq. */
class SpectralDeEsser
{
public:
    void prepare (double sampleRate, int numChannels);
    void reset();
    /** srcStart: posicao (amostras da fonte) da 1a amostra; srcStep: avanco por amostra */
    void process (float* const* channels, int numChannels, int startInBuffer, int numSamples,
                  const GainCurve& curve, double srcStart, double srcStep);

private:
    static constexpr int maxCh = 8;
    double fs = 48000.0;
    Biquad filt[maxCh];
    bool active = false;
};

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

    float levelGainDb = 0.0f;
    float targetGainDb = 0.0f;
    float rampFrom = 1.0f, rampTo = 1.0f;
    int   rampPos = 0;
    float currentGainDb = 0.0f;
    float inShortLufs = kMinDb;
    float lastTarget = -23.0f;
};

} // namespace vx
