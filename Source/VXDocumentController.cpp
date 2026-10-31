#include "VXDocumentController.h"

using namespace juce;

//==============================================================================
//  Thread de analise
//==============================================================================
class VXAudioSource::AnalysisThread : public Thread
{
public:
    explicit AnalysisThread (VXAudioSource& o) : Thread ("VX_Leveler analysis"), owner (o) {}

    void run() override
    {
        auto* r = owner.reader.get();
        if (r == nullptr) { owner.state = failed; return; }

        const int64 total = owner.getSampleCount();
        const int   nch   = jlimit (1, 2, (int) owner.getChannelCount());
        const int   chunk = 1 << 15;

        vx::Analyzer analyzer (owner.getSampleRate(), nch, total);
        AudioBuffer<float> buffer (nch, chunk);

        for (int64 pos = 0; pos < total; pos += chunk)
        {
            if (threadShouldExit()) { owner.state = needsAnalysis; return; }

            const int n = (int) jmin<int64> (chunk, total - pos);
            buffer.clear();

            bool ok = r->read (&buffer, 0, n, pos, true, nch > 1);
            if (! ok)
            {
                // o host pode estar ocupado: tenta de novo algumas vezes
                for (int attempt = 0; attempt < 20 && ! ok && ! threadShouldExit(); ++attempt)
                {
                    wait (50);
                    ok = r->read (&buffer, 0, n, pos, true, nch > 1);
                }
                if (! ok) { owner.state = failed; return; }
            }

            analyzer.process (buffer.getArrayOfReadPointers(), nch, n);
            owner.progress = (float) ((double) (pos + n) / (double) jmax<int64> (1, total));
        }

        std::shared_ptr<const vx::Analysis> result = analyzer.finish();
        std::atomic_store (&owner.analysis, result);
        owner.analysisVersion.fetch_add (1);
        owner.progress = 1.0f;
        owner.state = ready;
    }

private:
    VXAudioSource& owner;
};

//==============================================================================
VXAudioSource::VXAudioSource (ARA::PlugIn::Document* doc, ARA::ARAAudioSourceHostRef hostRef)
    : ARAAudioSource (doc, hostRef)
{
}

VXAudioSource::~VXAudioSource()
{
    stopAnalysis();
}

String VXAudioSource::getDisplayName() const
{
    return convertOptionalARAString (getName(), "Audio");
}

void VXAudioSource::stopAnalysis()
{
    if (thread != nullptr)
    {
        thread->signalThreadShouldExit();
        thread->stopThread (8000);
        thread.reset();
    }
    reader.reset();
}

void VXAudioSource::startAnalysisIfNeeded()
{
    JUCE_ASSERT_MESSAGE_THREAD

    // thread terminou: libera leitor/thread na thread de mensagens
    if (thread != nullptr && ! thread->isThreadRunning())
    {
        thread.reset();
        reader.reset();
    }

    const auto s = getState();
    if (s == analysing || s == ready)
        return;

    if (s == failed)
    {
        if (retries >= 3) return;
        ++retries;
    }

    if (! isSampleAccessEnabled() || getSampleCount() <= 0 || getSampleRate() <= 0.0)
        return;

    stopAnalysis();
    reader = std::make_unique<ARAAudioSourceReader> (this);
    if (! reader->isValid())
    {
        reader.reset();
        return;
    }

    progress = 0.0f;
    state = analysing;
    thread = std::make_unique<AnalysisThread> (*this);
    thread->startThread (Thread::Priority::low);
}

void VXAudioSource::invalidateAnalysis()
{
    JUCE_ASSERT_MESSAGE_THREAD
    stopAnalysis();
    std::atomic_store (&analysis, std::shared_ptr<const vx::Analysis>());
    analysisVersion.fetch_add (1);
    retries = 0;
    state = needsAnalysis;
}

//==============================================================================
VXAudioModification::VXAudioModification (ARAAudioSource* audioSource,
                                          ARA::ARAAudioModificationHostRef hostRef,
                                          const ARAAudioModification* clone)
    : ARAAudioModification (audioSource, hostRef, clone)
{
    if (auto* c = dynamic_cast<const VXAudioModification*> (clone))
        editsDb = c->editsDb;
}

void VXAudioModification::notifyEdited()
{
    ++editsVersion;
    notifyContentChanged (ARAContentUpdateScopes::samplesAreAffected(), true);
    for (auto* region : getPlaybackRegions())
        region->notifyContentChanged (ARAContentUpdateScopes::samplesAreAffected(), true);
}

String VXAudioModification::getDisplayName() const
{
    auto name = convertOptionalARAString (getEffectiveName());
    if (name.isEmpty())
        if (auto* src = getAudioSource<VXAudioSource>())
            name = src->getDisplayName();
    return name;
}

//==============================================================================
//  Renderer
//==============================================================================
VXPlaybackRenderer::VXPlaybackRenderer (ARA::PlugIn::DocumentController* dc, VXDocumentController& o)
    : ARAPlaybackRenderer (dc), owner (o)
{
}

void VXPlaybackRenderer::prepareToPlay (double sr, int maxBlockIn, int numChannelsIn,
                                        AudioProcessor::ProcessingPrecision precision,
                                        AlwaysNonRealtime alwaysNonRealtime)
{
    ARAPlaybackRenderer::prepareToPlay (sr, maxBlockIn, numChannelsIn, precision, alwaysNonRealtime);

    sampleRate  = sr;
    maxBlock    = maxBlockIn;
    numChannels = numChannelsIn;
    tempBuffer  = std::make_unique<AudioBuffer<float>> (numChannels, maxBlock);

    const bool buffered = alwaysNonRealtime == AlwaysNonRealtime::no;
    bool mismatch = false;

    for (auto* region : getPlaybackRegions())
    {
        auto* source = region->getAudioModification()->getAudioSource();
        if (! approximatelyEqual (source->getSampleRate(), sampleRate)) mismatch = true;

        if (readers.find (source) != readers.end())
            continue;

        auto reader = std::make_unique<ARAAudioSourceReader> (source);
        Reader r;
        if (buffered)
        {
            const int readAhead = jmax (4 * maxBlock, roundToInt (2.0 * sampleRate));
            auto* b = new BufferingAudioReader (reader.release(), *readThread, readAhead);
            r.buffering = b;
            r.reader.reset (b);
        }
        else
        {
            r.reader = std::move (reader);
        }
        readers.emplace (source, std::move (r));
    }
    sampleRateMismatch = mismatch;
}

void VXPlaybackRenderer::releaseResources()
{
    readers.clear();
    tempBuffer.reset();
    ARAPlaybackRenderer::releaseResources();
}

void VXPlaybackRenderer::publishCurves (std::shared_ptr<const VXCurveMap> map)
{
    JUCE_ASSERT_MESSAGE_THREAD
    keepAlive.push_back (map);
    while (keepAlive.size() > 8) keepAlive.pop_front();
    std::atomic_store (&curves, map);
}

bool VXPlaybackRenderer::processBlock (AudioBuffer<float>& buffer,
                                       AudioProcessor::Realtime realtime,
                                       const AudioPlayHead::PositionInfo& positionInfo) noexcept
{
    const auto lock = owner.getProcessingLock();
    if (! lock.isLocked() || tempBuffer == nullptr)
    {
        buffer.clear();
        return true;
    }

    const int numSamples = buffer.getNumSamples();
    const int nch = jmin (buffer.getNumChannels(), tempBuffer->getNumChannels());
    if (numSamples > tempBuffer->getNumSamples())
    {
        buffer.clear();
        return false;
    }

    buffer.clear();
    if (! positionInfo.getIsPlaying())
        return true;

    const auto curveMap = std::atomic_load (&curves);
    const bool bypass = bypassed.load();
    const float outGain = outputGain.load();
    const int64 timeInSamples = positionInfo.getTimeInSamples().orFallback (0);
    const auto blockRange = Range<int64>::withStartAndLength (timeInSamples, numSamples);

    bool success = true;
    float lastGain = 0.0f;
    bool anyGain = false;

    for (auto* region : getPlaybackRegions())
    {
        const auto playbackRange = region->getSampleRange (sampleRate, ARAPlaybackRegion::IncludeHeadAndTail::no);
        auto renderRange = blockRange.getIntersectionWith (playbackRange);
        if (renderRange.isEmpty()) continue;

        const Range<int64> modRange { region->getStartInAudioModificationSamples(),
                                      region->getEndInAudioModificationSamples() };
        const int64 offset = modRange.getStart() - playbackRange.getStart();
        renderRange = renderRange.getIntersectionWith (modRange.movedToStartAt (playbackRange.getStart()));
        if (renderRange.isEmpty()) continue;

        auto* mod = region->getAudioModification();
        auto* source = mod->getAudioSource();
        auto it = readers.find (source);
        if (it == readers.end()) { success = false; continue; }

        auto& rd = it->second;
        if (rd.buffering != nullptr)
            rd.buffering->setReadTimeout (realtime == AudioProcessor::Realtime::no ? 200 : 0);

        const int n = (int) renderRange.getLength();
        const int startInBuffer = (int) (renderRange.getStart() - blockRange.getStart());
        const int64 startInSource = renderRange.getStart() + offset;

        if (! rd.reader->read (tempBuffer.get(), startInBuffer, n, startInSource, true, true))
        {
            success = false;
            continue;
        }

        // curva de ganho do clipe
        const vx::GainCurve* curve = nullptr;
        if (curveMap != nullptr && ! bypass)
        {
            auto ci = curveMap->find (mod);
            if (ci != curveMap->end() && ci->second != nullptr) curve = ci->second.get();
        }

        // posicao na fonte -> amostras da curva (mesma taxa da fonte)
        const double srcScale = curve != nullptr ? curve->sampleRate / sampleRate : 1.0;

        for (int c = 0; c < nch; ++c)
        {
            auto* src = tempBuffer->getReadPointer (c, startInBuffer);
            auto* dst = buffer.getWritePointer (c, startInBuffer);

            if (curve != nullptr)
            {
                for (int i = 0; i < n; ++i)
                    dst[i] += src[i] * curve->gainAtSample ((double) (startInSource + i) * srcScale) * outGain;
            }
            else
            {
                for (int i = 0; i < n; ++i)
                    dst[i] += src[i] * (bypass ? 1.0f : outGain);
            }
        }

        if (curve != nullptr)
        {
            lastGain = 20.0f * std::log10 (jmax (1.0e-6f, curve->gainAtSample ((double) startInSource * srcScale)));
            anyGain = true;
        }
    }

    currentGainDb = anyGain ? lastGain : 0.0f;
    return success;
}

//==============================================================================
//  Document controller
//==============================================================================
void VXDocumentController::willBeginEditing (ARADocument* doc)
{
    processLock.enterWrite();
    document = doc;
    isEditing = true;
}

void VXDocumentController::didEndEditing (ARADocument* doc)
{
    document = doc;
    isEditing = false;
    processLock.exitWrite();
    ++modelVersion;
    scheduleAnalyses();
    if (! isTimerRunning()) startTimer (300);
}

ARAAudioSource* VXDocumentController::doCreateAudioSource (ARADocument* doc, ARA::ARAAudioSourceHostRef hostRef) noexcept
{
    return new VXAudioSource (doc, hostRef);
}

ARAAudioModification* VXDocumentController::doCreateAudioModification (ARAAudioSource* audioSource,
                                                                       ARA::ARAAudioModificationHostRef hostRef,
                                                                       const ARAAudioModification* clone) noexcept
{
    return new VXAudioModification (audioSource, hostRef, clone);
}

ARAPlaybackRenderer* VXDocumentController::doCreatePlaybackRenderer() noexcept
{
    return new VXPlaybackRenderer (getDocumentController(), *this);
}

void VXDocumentController::doUpdateAudioSourceContent (ARAAudioSource* audioSource, ARAContentUpdateScopes scopeFlags)
{
    if (scopeFlags.affectSamples())
        if (auto* s = dynamic_cast<VXAudioSource*> (audioSource))
            s->invalidateAnalysis();
    ++modelVersion;
}

void VXDocumentController::didEnableAudioSourceSamplesAccess (ARAAudioSource*, bool enable)
{
    if (enable && ! isEditing)
        scheduleAnalyses();
}

void VXDocumentController::scheduleAnalyses()
{
    if (document == nullptr || isEditing) return;
    for (auto* s : document->getAudioSources())
        if (auto* vs = dynamic_cast<VXAudioSource*> (s))
            vs->startAnalysisIfNeeded();
}

void VXDocumentController::timerCallback()
{
    scheduleAnalyses();
}

//------------------------------------------------------------------------------
static constexpr int kArchiveMagic = 0x56584c31;   // "VXL1"

bool VXDocumentController::doStoreObjectsToStream (ARAOutputStream& output, const ARAStoreObjectsFilter* filter) noexcept
{
    const auto& mods = filter->getAudioModificationsToStore<VXAudioModification>();

    if (! output.writeInt (kArchiveMagic)) return false;
    if (! output.writeInt (1)) return false;   // versao
    if (! output.writeInt64 ((int64) mods.size())) return false;

    for (size_t i = 0; i < mods.size(); ++i)
    {
        auto* m = mods[i];
        if (! output.writeString (String (m->getPersistentID()))) return false;

        const bool any = m->hasEdits();
        const int n = any ? (int) m->editsDb.size() : 0;
        if (! output.writeInt (n)) return false;
        if (n > 0 && ! output.write (m->editsDb.data(), (size_t) n * sizeof (float))) return false;

        getDocumentController()->getHostArchivingController()->notifyDocumentArchivingProgress ((float) i / (float) jmax<size_t> (1, mods.size()));
    }
    getDocumentController()->getHostArchivingController()->notifyDocumentArchivingProgress (1.0f);
    return true;
}

bool VXDocumentController::doRestoreObjectsFromStream (ARAInputStream& input, const ARARestoreObjectsFilter* filter) noexcept
{
    if (input.readInt() != kArchiveMagic) return ! input.failed();
    const int version = input.readInt();
    ignoreUnused (version);
    const auto count = input.readInt64();

    for (int64 i = 0; i < count && ! input.failed(); ++i)
    {
        const String persistentID = input.readString();
        const int n = input.readInt();
        std::vector<float> edits;
        if (n > 0 && n < 100000000)
        {
            edits.resize ((size_t) n);
            if (input.read (edits.data(), (int) ((size_t) n * sizeof (float))) != (int) ((size_t) n * sizeof (float)))
                return false;
        }

        if (auto* m = filter->getAudioModificationToRestoreStateWithID<VXAudioModification> (persistentID.getCharPointer()))
        {
            m->editsDb = std::move (edits);
            ++m->editsVersion;
            m->notifyContentChanged (ARAContentUpdateScopes::samplesAreAffected(), false);
        }
        getDocumentController()->getHostArchivingController()->notifyDocumentUnarchivingProgress ((float) i / (float) jmax<int64> (1, count));
    }
    getDocumentController()->getHostArchivingController()->notifyDocumentUnarchivingProgress (1.0f);
    return ! input.failed();
}

//==============================================================================
const ARA::ARAFactory* JUCE_CALLTYPE createARAFactory()
{
    return juce::ARADocumentControllerSpecialisation::createARAFactory<VXDocumentController>();
}
