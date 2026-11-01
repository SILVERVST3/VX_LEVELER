#pragma once
/*
    VX_Leveler SILVER - camada ARA 2

    VXAudioSource       : guarda a analise (quadros de 10 ms) de cada audio do host.
                          A analise roda numa thread propria lendo as amostras via ARA.
    VXAudioModification : guarda as edicoes manuais (dB por quadro) de cada clipe.
                          Sao salvas dentro do projeto do host (arquivo ARA).
    VXPlaybackRenderer  : toca os clipes aplicando a curva de ganho.
    VXDocumentController: cria os objetos acima, salva/restaura e agenda as analises.
*/

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <unordered_map>
#include <deque>
#include "LevelerDSP.h"

class VXDocumentController;

/** thread compartilhada de leitura antecipada (BufferingAudioReader) */
class VXReadThread : public juce::TimeSliceThread
{
public:
    VXReadThread() : juce::TimeSliceThread ("VX_Leveler ARA read") { startThread (juce::Thread::Priority::high); }
};

//==============================================================================
class VXAudioSource : public juce::ARAAudioSource
{
public:
    enum State { needsAnalysis = 0, analysing, ready, failed };

    VXAudioSource (ARA::PlugIn::Document* document, ARA::ARAAudioSourceHostRef hostRef);
    ~VXAudioSource() override;

    /** thread de mensagens */
    void startAnalysisIfNeeded();
    void invalidateAnalysis();
    /** analise recuperada do projeto (nao precisa ler o audio de novo) */
    void setRestoredAnalysis (std::shared_ptr<const vx::Analysis> a);

    std::shared_ptr<const vx::Analysis> getAnalysis() const   { return std::atomic_load (&analysis); }
    int   getAnalysisVersion() const noexcept                  { return analysisVersion.load(); }
    State getState() const noexcept                            { return (State) state.load(); }
    float getProgress() const noexcept                         { return progress.load(); }
    juce::String getDisplayName() const;

private:
    class AnalysisThread;
    void stopAnalysis();

    std::shared_ptr<const vx::Analysis> analysis;
    std::atomic<int>   analysisVersion { 0 };
    std::atomic<int>   state { needsAnalysis };
    std::atomic<float> progress { 0.0f };
    std::unique_ptr<juce::ARAAudioSourceReader> reader;
    std::unique_ptr<AnalysisThread> thread;
    int retries = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VXAudioSource)
};

//==============================================================================
class VXAudioModification : public juce::ARAAudioModification
{
public:
    VXAudioModification (juce::ARAAudioSource* audioSource,
                         ARA::ARAAudioModificationHostRef hostRef,
                         const juce::ARAAudioModification* optionalModificationToClone);

    /** dB por quadro de 10 ms (tempo da fonte). Somente na thread de mensagens. */
    std::vector<float> editsDb;
    int editsVersion = 0;

    /** nos da ferramenta "Nos": envelope em dB (tempo da fonte, segundos), ordenado por tempo.
        Entre nos: linear. Antes do primeiro / depois do ultimo: mantem o valor do no da ponta. */
    struct Node { double t = 0.0; float db = 0.0f; };
    std::vector<Node> nodes;

    float nodeEnvAt (double t) const noexcept
    {
        if (nodes.empty()) return 0.0f;
        if (t <= nodes.front().t) return nodes.front().db;
        if (t >= nodes.back().t)  return nodes.back().db;
        for (size_t i = 1; i < nodes.size(); ++i)
            if (t < nodes[i].t)
            {
                const auto& a = nodes[i - 1];
                const auto& b = nodes[i];
                const double u = (t - a.t) / std::max (1.0e-9, b.t - a.t);
                return (float) (a.db + (b.db - a.db) * u);
            }
        return nodes.back().db;
    }

    /** regioes (pausa/respiro/sibilante) desligadas pelo usuario */
    std::vector<vx::RegionKey> disabledRegions;
    int regionsVersion = 0;

    void toggleRegion (vx::RegionType type, int startFrame)
    {
        for (auto it = disabledRegions.begin(); it != disabledRegions.end(); ++it)
            if (it->type == (int) type && std::abs (it->startFrame - startFrame) <= 6)
            {
                disabledRegions.erase (it);
                ++regionsVersion;
                return;
            }
        disabledRegions.push_back ({ (int) type, startFrame });
        ++regionsVersion;
    }

    void ensureSize (int numFrames)
    {
        if ((int) editsDb.size() < numFrames) editsDb.resize ((size_t) numFrames, 0.0f);
    }

    bool hasEdits() const
    {
        for (auto v : editsDb) if (std::abs (v) > 1.0e-6f) return true;
        return ! nodes.empty();
    }

    /** true quando o estado veio do arquivo ARA do projeto */
    bool restoredFromArchive = false;

    /** avisa o host que o audio deste clipe mudou (apos uma edicao manual) */
    void notifyEdited();

    juce::String getDisplayName() const;
};

//==============================================================================
/** Estado manual de um clipe (pincel, nos, regioes desligadas) - salvar/ler. */
struct VXModState
{
    std::vector<float> edits;
    std::vector<vx::RegionKey> disabled;
    std::vector<VXAudioModification::Node> nodes;

    static VXModState from (const VXAudioModification& m);
    void applyTo (VXAudioModification& m) const;
    bool isEmpty() const;
    void write (juce::OutputStream& out) const;      // edicoes em trechos (so o que nao e zero)
    bool read (juce::InputStream& in);
};

/** Analise <-> bytes compactados (gzip) */
juce::MemoryBlock vxSerialiseAnalysis (const vx::Analysis& a);
std::shared_ptr<vx::Analysis> vxDeserialiseAnalysis (const void* data, size_t size);

//==============================================================================
using VXCurveMap = std::unordered_map<const juce::ARAAudioModification*, std::shared_ptr<const vx::GainCurve>>;

class VXPlaybackRenderer : public juce::ARAPlaybackRenderer
{
public:
    VXPlaybackRenderer (ARA::PlugIn::DocumentController* dc, VXDocumentController& owner);

    void prepareToPlay (double sampleRate, int maximumSamplesPerBlock, int numChannels,
                        juce::AudioProcessor::ProcessingPrecision precision,
                        AlwaysNonRealtime alwaysNonRealtime) override;
    void releaseResources() override;

    bool processBlock (juce::AudioBuffer<float>& buffer,
                       juce::AudioProcessor::Realtime realtime,
                       const juce::AudioPlayHead::PositionInfo& positionInfo) noexcept override;
    using juce::ARAPlaybackRenderer::processBlock;

    /** thread de mensagens: publica um novo conjunto de curvas */
    void publishCurves (std::shared_ptr<const VXCurveMap> map);

    std::atomic<float> outputGain { 1.0f };
    std::atomic<bool>  bypassed { false };
    std::atomic<float> currentGainDb { 0.0f };
    std::atomic<bool>  sampleRateMismatch { false };

private:
    struct Reader
    {
        std::unique_ptr<juce::AudioFormatReader> reader;
        juce::BufferingAudioReader* buffering = nullptr;
    };

    VXDocumentController& owner;
    juce::SharedResourcePointer<VXReadThread> readThread;
    std::map<juce::ARAAudioSource*, Reader> readers;
    std::unique_ptr<juce::AudioBuffer<float>> tempBuffer;
    double sampleRate = 48000.0;
    int maxBlock = 512, numChannels = 2;

    std::shared_ptr<const VXCurveMap> curves;

    struct DeEssState
    {
        vx::SpectralDeEsser filter;
        juce::int64 expectedSource = -1;
    };
    std::map<const juce::ARAPlaybackRegion*, DeEssState> deEssers;
    std::deque<std::shared_ptr<const VXCurveMap>> keepAlive;   // evita liberar memoria na thread de audio
};

//==============================================================================
class VXDocumentController : public juce::ARADocumentControllerSpecialisation
{
public:
    // construtor/destrutor explicitos (definidos no .cpp) - o MSVC falha com o construtor herdado
    VXDocumentController (const ARA::PlugIn::PlugInEntry* entry,
                          const ARA::ARADocumentControllerHostInstance* instance);
    ~VXDocumentController() override;

    juce::ScopedTryReadLock getProcessingLock() { return juce::ScopedTryReadLock { processLock }; }
    juce::ARADocument* getDocumentPtr() const noexcept { return document; }

    /** inicia a analise das fontes com acesso liberado (thread de mensagens) */
    void scheduleAnalyses();
    /** reinicia a analise de todas as fontes */
    void reanalyseAll();
    /** texto de diagnostico para a interface */
    juce::String getDiagnostics() const;

    /** contador que muda sempre que o modelo ARA muda (editores redesenham) */
    std::atomic<int> modelVersion { 0 };

protected:
    void willBeginEditing (juce::ARADocument*) override;
    void didEndEditing (juce::ARADocument*) override;

    juce::ARAAudioSource* doCreateAudioSource (juce::ARADocument* document,
                                               ARA::ARAAudioSourceHostRef hostRef) noexcept override;
    juce::ARAAudioModification* doCreateAudioModification (juce::ARAAudioSource* audioSource,
                                                           ARA::ARAAudioModificationHostRef hostRef,
                                                           const juce::ARAAudioModification* optionalModificationToClone) noexcept override;
    juce::ARAPlaybackRenderer* doCreatePlaybackRenderer() noexcept override;

    bool doRestoreObjectsFromStream (juce::ARAInputStream& input, const juce::ARARestoreObjectsFilter* filter) noexcept override;
    bool doStoreObjectsToStream (juce::ARAOutputStream& output, const juce::ARAStoreObjectsFilter* filter) noexcept override;

    void doUpdateAudioSourceContent (juce::ARAAudioSource* audioSource, juce::ARAContentUpdateScopes scopeFlags) override;
    void didEnableAudioSourceSamplesAccess (juce::ARAAudioSource* audioSource, bool enable) override;

private:
    friend class VXAnalysisScheduler;
    std::unique_ptr<juce::Timer> scheduler;   // timer fica no .cpp (pimpl)

    juce::ReadWriteLock processLock;
    juce::ARADocument* document = nullptr;
    bool isEditing = false;
};
