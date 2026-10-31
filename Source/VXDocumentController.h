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

    void ensureSize (int numFrames)
    {
        if ((int) editsDb.size() < numFrames) editsDb.resize ((size_t) numFrames, 0.0f);
    }

    bool hasEdits() const
    {
        for (auto v : editsDb) if (std::abs (v) > 1.0e-6f) return true;
        return false;
    }

    /** avisa o host que o audio deste clipe mudou (apos uma edicao manual) */
    void notifyEdited();

    juce::String getDisplayName() const;
};

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
    void scheduleAnalyses();
    std::unique_ptr<juce::Timer> scheduler;   // timer fica no .cpp (pimpl)

    juce::ReadWriteLock processLock;
    juce::ARADocument* document = nullptr;
    bool isEditing = false;
};
