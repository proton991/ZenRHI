#include "ZenRHI/RHIDiagnostics.h"
#include <span>
#include "ZenRHI/RHICommandListExecutor.h"
#include "ZenFoundation/Utils/Errors.h"
#include <algorithm>

namespace zen
{
namespace
{
// Ordered backend call. A rejected or cancelled job yields a value-initialized result.
template <typename Value, typename... Arguments> Value InvokeOrDefault(RHIThread& thread, Arguments&&... arguments)
{
    RHIResult<Value> result = thread.InvokeChecked(std::forward<Arguments>(arguments)...);

    return result ? result.GetValue() : Value{};
}
} // namespace

// Both the command arena and referenced resources survive until GPU retirement.
// This conservative first implementation can later recycle the arena after translation.
struct RHICommandBatch : RefCounted
{
    struct Group
    {
        RHICommandListPtr               commands;
        std::vector<RHISubmissionPoint> predecessors;
    };
    HeapVector<Group>                     groups;
    RHICommandListPtr                     presentCommands;
    RHIResourceReferences                 resources;
    RHISubmissionStatePtr                 submissionState;
    RHIViewport*                          viewport{nullptr};
    std::promise<RHIBatchResult>          completion;
    RefCountPtr<RHIThreadEvent>           completionEvent{MakeRefCountPtr<RHIThreadEvent>()};
    std::chrono::steady_clock::time_point queuedAt;
    RHIBatchResult                        result;
    bool                                  executionFinished{false};
    bool                                  endFrame{true};
    bool                                  asynchronous{true};

    RHICommandBatch()                                  = default;

    RHICommandBatch(const RHICommandBatch&)            = delete;

    RHICommandBatch& operator=(const RHICommandBatch&) = delete;

    RHICommandBatch(RHICommandBatch&&)                 = delete;

    RHICommandBatch& operator=(RHICommandBatch&&)      = delete;
};


RHISubmissionState::RHISubmissionState(std::span<const RHICommandContextType> queues)
{
    for (RHICommandContextType queue : queues)
    {
        m_groups.push_back({{queue, 0}, false});

        m_failed |= size_t(queue) >= RHICompletionSet::kQueueCount;
    }
}

bool RHISubmissionState::Queue()
{
    std::lock_guard<std::mutex> lock(m_mutex);

    const bool result = !m_queued && !m_failed;

    if (result)
    {
        m_queued = true;
    }

    return result;
}

bool RHISubmissionState::IsQueued() const
{
    std::lock_guard<std::mutex> lock(m_mutex);

    return m_queued && !m_failed;
}

bool RHISubmissionState::Matches(uint32_t group, RHICommandContextType queue) const
{
    // Only the serial/accepted fields are mutable; queue and vector storage are immutable.
    return group < m_groups.size() && m_groups[group].submission.queue == queue;
}

size_t RHISubmissionState::GetGroupCount() const
{
    return m_groups.size();
}

bool RHISubmissionState::Accept(uint32_t group, RHISubmissionDependency submission)
{
    std::lock_guard<std::mutex> lock(m_mutex);

    const bool result = m_queued && !m_failed && Matches(group, submission.queue)
                     && submission.serial != RHISubmissionDependency::kLatestSubmitted && !m_groups[group].accepted;

    if (result)
    {
        m_groups[group].submission.serial = submission.serial;

        m_groups[group].accepted          = true;
    }

    return result;
}

void RHISubmissionState::Fail()
{
    std::lock_guard<std::mutex> lock(m_mutex);

    m_failed = true;
}

bool RHISubmissionState::FinishSubmission()
{
    std::lock_guard<std::mutex> lock(m_mutex);

    bool result = m_queued && !m_failed;

    for (const Group& group : m_groups)
    {
        result &= group.accepted;
    }

    m_submissionFinished = result;

    return result;
}

bool RHISubmissionState::IsSubmissionFinished() const
{
    std::lock_guard<std::mutex> lock(m_mutex);

    return m_submissionFinished && !m_failed;
}

RHISubmissionPointStatus RHISubmissionState::Resolve(uint32_t group, RHISubmissionDependency& submission) const
{
    std::lock_guard<std::mutex> lock(m_mutex);

    RHISubmissionPointStatus result = RHISubmissionPointStatus::eInvalid;

    if (group < m_groups.size())
    {
        if (m_failed)
        {
            result = RHISubmissionPointStatus::eFailed;
        }
        else if (m_groups[group].accepted)
        {
            submission = m_groups[group].submission;

            result     = RHISubmissionPointStatus::eAccepted;
        }
        else
        {
            result = RHISubmissionPointStatus::ePending;
        }
    }

    return result;
}

bool RHISubmissionPoint::IsValid() const
{
    return size_t(queue) < RHICompletionSet::kQueueCount
        && (state ? serial == 0 && state->Matches(group, queue)
                  : serial != 0 && serial != RHISubmissionDependency::kLatestSubmitted && group == UINT32_MAX);
}

bool RHISubmissionPoint::operator==(const RHISubmissionPoint& other) const
{
    return queue == other.queue && serial == other.serial && state.Get() == other.state.Get() && group == other.group;
}

RHISubmissionPointStatus RHISubmissionPoint::Resolve(RHISubmissionDependency& submission) const
{
    RHISubmissionPointStatus result = RHISubmissionPointStatus::eInvalid;

    if (IsValid())
    {
        if (state)
        {
            result = state->Resolve(group, submission);
        }
        else
        {
            submission = {queue, serial};

            result     = RHISubmissionPointStatus::eAccepted;
        }
    }

    return result;
}

RHISubmissionTicket::RHISubmissionTicket(std::shared_future<RHIBatchResult> result,
                                         RefCountPtr<RHIThreadEvent>        completion,
                                         RHIDeviceRuntimePtr                runtime) :
    m_result(std::move(result)), m_completion(std::move(completion)), m_runtime(std::move(runtime))
{}

bool RHISubmissionTicket::IsValid() const
{
    return m_result.valid();
}

bool RHISubmissionTicket::IsReady() const
{
    return m_result.valid() && m_result.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
}

const RHIBatchResult& RHISubmissionTicket::Wait() const
{
    ZEN_RHI_VERIFY(m_result.valid());

    if (m_result.valid() && !IsReady())
    {
        m_completion->Wait("frame submission", m_runtime->thread.GetActivity());
    }

    return m_result.get();
}

RHICommandListExecutor::RHICommandListExecutor(DynamicRHI* backend, RHIExecutionMode mode, uint32_t framesInFlight) :
    m_backend(backend),
    m_runtime(&backend->GetRuntime()),
    m_mode(mode),
    m_renderThread(std::this_thread::get_id()),
    m_gpuInfo(backend->QueryGPUInfo()),
    m_apiType(backend->GetAPIType()),
    m_name(backend->GetName()),
    m_depthFormat(backend->GetSupportedDepthFormat()),
    m_submissionQueueCapabilities(backend->GetQueueCapabilities())
{
    for (uint32_t i = 0; i < RHICompletionSet::kQueueCount; ++i)
    {
        m_queueCapabilities.push_back(backend->GetQueueCopyCapabilities(static_cast<RHICommandContextType>(i)));

        m_queueProgress.push_back(MakeUnique<QueueProgress>());
    }

    m_runtime->frameState.Init(framesInFlight);

    m_runtime->thread.Start(mode);

    m_runtime->thread.Invoke(&RHICommandListExecutor::PublishProgress, this);
}

RHICommandListExecutor::~RHICommandListExecutor()
{
    if (!m_destroyed)
    {
        Destroy();
    }

    DeleteBackend();
}

void RHICommandListExecutor::DeleteBackend()
{
    ZEN_DELETE(m_backend);

    m_backend = nullptr;
}

void RHICommandListExecutor::Destroy()
{
    if (!m_destroyed)
    {
        m_runtime->thread.Stop(std::bind_front(&RHICommandListExecutor::ExecuteDestroy, this));

        m_destroyed = true;
    }
}

void RHICommandListExecutor::ExecuteDestroy()
{
    ExecuteWaitIdle();

    CollectCompletedBatches(true);
    {
        std::lock_guard<std::mutex> lock(m_recordingCommandListMutex);

        m_recordingCommandLists.clear();
    }

    m_presentCommandLists.clear();

    m_backend->Destroy();
}

void RHICommandListExecutor::PublishProgress()
{
    m_runtime->thread.CheckOwnership();
    m_publishedFrameNumber.store(ToValue(m_runtime->frameState.GetFrameNumber()), std::memory_order_release);

    for (uint32_t i = 0; i < RHICompletionSet::kQueueCount; ++i)
    {
        const RHICommandContextType type = static_cast<RHICommandContextType>(i);

        m_queueProgress[i]->submitted.store(m_backend->GetLastSubmittedSerial(type), std::memory_order_release);
    }

    // A completion query may fail after work was accepted on another queue.
    // Capture every submitted serial before querying completion for any queue.
    for (uint32_t i = 0; i < RHICompletionSet::kQueueCount; ++i)
    {
        const RHICommandContextType type = static_cast<RHICommandContextType>(i);

        m_queueProgress[i]->completed.store(m_backend->QueryLastCompletedSerial(type), std::memory_order_release);
    }

    PublishSubmissionStatus();
}

void RHICommandListExecutor::PublishSubmissionStatus()
{
    m_runtime->thread.CheckOwnership();

    if (m_backend->AreSubmissionsBlocked())
    {
        PublishTerminalError(m_backend->GetTerminalError());

        if (m_backend->HasDeviceLoss())
        {
            m_runtime->thread.Fail(MakeRHIError(RHIErrorCode::eDeviceLost, "RHI device loss observed", __FILE__, __LINE__));
        }
    }
}

void RHICommandListExecutor::PublishTerminalError(RHIError error)
{
    if (!error.IsFailure())
    {
        error = MakeRHIError(RHIErrorCode::eBackendFailure, "RHI operation failed", __FILE__, __LINE__);
    }

    bool first = false;
    {
        std::lock_guard<std::mutex> lock(m_errorMutex);

        first = !m_terminalError.IsFailure();

        if (first)
        {
            m_terminalError = error;
        }
    }

    m_blocked.store(true, std::memory_order_release);

    m_runtime->thread.Fail(error);

    if (first)
    {
        const RHICompletionSet submitted = GetCachedSubmittedSerials();
        // Rejected admission can log on the producer while the worker is advancing a frame.
        const uint64_t frame = m_publishedFrameNumber.load(std::memory_order_acquire);

        ZEN_RHI_LOG_ERROR(
            "RHI stopped at frame {} (slot {}): {} (code {}, native {}, {}:{}, resource {}); submitted graphics={}, compute={}, transfer={}",
            frame, frame % m_runtime->frameState.GetNumFramesInFlight(), RHIErrorText(error.operation, "unknown operation"),
            static_cast<uint32_t>(error.code), error.nativeCode, RHIErrorText(error.source, "unknown source"), error.line,
            error.resourceId, submitted.serials[static_cast<uint32_t>(RHICommandContextType::eGraphics)],
            submitted.serials[static_cast<uint32_t>(RHICommandContextType::eAsyncCompute)],
            submitted.serials[static_cast<uint32_t>(RHICommandContextType::eTransfer)]);
    }
}

RHIError RHICommandListExecutor::GetTerminalError() const
{
    std::lock_guard<std::mutex> lock(m_errorMutex);

    return RHIErrorOr(m_terminalError, m_runtime->thread.GetFailure());
}

void RHICommandListExecutor::ReleaseWhenComplete(RHIResourcePtr<RHIResource> resource)
{
    if (resource)
    {
        // Ordered after every batch dispatched before it, so the serials it captures cover them. The caller does not
        // wait. The cleanup queue stays open while the RHI thread drains or after a terminal error, so the reference
        // is never dropped while earlier submissions may still use the resource.
        m_runtime->thread.DispatchCleanup(
            std::bind_front(&RHICommandListExecutor::RetainUntilComplete, this, std::move(resource)));
    }
}

void RHICommandListExecutor::RetainUntilComplete(RHIResourcePtr<RHIResource> resource)
{
    PublishProgress();
    m_deferredResources.push_back({GetCachedSubmittedSerials(), std::move(resource)});
    CollectCompletedBatches();
}

void RHICommandListExecutor::CollectCompletedBatches(bool force)
{
    const RHICompletionSet progress = GetCachedCompletedSerials();

    for (HeapVector<DeferredResource>::iterator it = m_deferredResources.begin(); it != m_deferredResources.end();)
    {
        if (force || (!m_blocked.load(std::memory_order_acquire) && it->serials.IsCompleteAt(progress)))
        {
            it = m_deferredResources.erase(it);
        }
        else
        {
            ++it;
        }
    }
    for (HeapVector<RefCountPtr<RHICommandBatch>>::iterator it = m_retired.begin(); it != m_retired.end();)
    {
        const bool completed = (*it)->executionFinished && !m_blocked.load(std::memory_order_acquire)
                            && (*it)->result.requiredSerials.IsCompleteAt(progress);

        if (force || completed)
        {
            if (!force)
            {
                RecycleCommandLists(**it);
            }

            it = m_retired.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

RHICompletionSet RHICommandListExecutor::GetCachedSubmittedSerials() const
{
    RHICompletionSet progress;

    for (size_t i = 0; i < RHICompletionSet::kQueueCount; ++i)
    {
        progress.serials[i] = m_queueProgress[i]->submitted.load(std::memory_order_acquire);
    }

    return progress;
}

RHICompletionSet RHICommandListExecutor::GetCachedCompletedSerials() const
{
    RHICompletionSet progress;

    for (size_t i = 0; i < RHICompletionSet::kQueueCount; ++i)
    {
        progress.serials[i] = m_queueProgress[i]->completed.load(std::memory_order_acquire);
    }

    return progress;
}

RHICommandListPtr RHICommandListExecutor::AcquireRecordingCommandList(RHICommandContextType queue)
{
    RHICommandListPtr result;
    {
        std::lock_guard<std::mutex> lock(m_recordingCommandListMutex);

        HeapVector<RHICommandListPtr>& lists = m_recordingCommandLists[size_t(queue)];

        if (!lists.empty())
        {
            result = std::move(lists.back());

            lists.pop_back();
        }
    }

    return result;
}

RHICommandListPtr RHICommandListExecutor::AcquirePresentCommandList()
{
    m_runtime->thread.CheckOwnership();

    RHICommandListPtr result;

    if (!m_presentCommandLists.empty())
    {
        result = std::move(m_presentCommandLists.back());

        m_presentCommandLists.pop_back();
    }
    else
    {
        result.reset(RHICommandList::Create(m_backend->GetCommandContext(RHICommandContextType::eGraphics)));
    }

    return result;
}

void RHICommandListExecutor::RecycleCommandLists(RHICommandBatch& batch)
{
    m_runtime->thread.CheckOwnership();

    for (RHICommandBatch::Group& group : batch.groups)
    {
        const RHICommandContextType queue = group.commands->GetContext()->GetContextType();

        group.commands->ResetForReuse();

        std::lock_guard<std::mutex> lock(m_recordingCommandListMutex);

        HeapVector<RHICommandListPtr>& lists = m_recordingCommandLists[size_t(queue)];

        if (lists.size() < kMaxRecycledCommandLists)
        {
            lists.push_back(std::move(group.commands));
        }
    }

    if (batch.presentCommands != nullptr)
    {
        batch.presentCommands->Reset();

        if (m_presentCommandLists.size() < kMaxRecycledCommandLists)
        {
            m_presentCommandLists.push_back(std::move(batch.presentCommands));
        }
    }
}

RHISubmissionResult RHICommandListExecutor::ExecuteBatch(std::span<RHICommandList*> lists)
{
    m_runtime->thread.CheckOwnership();

    RHISubmissionResult result = RHISubmissionResult::eFatal;

    m_executionError           = {};

    for (RHICommandList* commands : lists)
    {
        if (commands != nullptr)
        {
            commands->SetSubmittedSerial(0);
        }
    }

    PublishSubmissionStatus();

    if (!m_blocked.load(std::memory_order_acquire))
    {
        result = m_backend->SubmitCommandLists(lists);

        if (result != RHISubmissionResult::eSuccess)
        {
            m_executionError = m_backend->GetLastSubmissionError();
        }

        if (result == RHISubmissionResult::eFatal)
        {
            PublishTerminalError(m_executionError);
        }
    }

    PublishProgress();

    if (AreSubmissionsBlocked())
    {
        result = RHISubmissionResult::eFatal;

        if (!m_executionError.IsFailure())
        {
            m_executionError = GetTerminalError();
        }
    }

    if (result != RHISubmissionResult::eSuccess && !m_executionError.IsFailure())
    {
        m_executionError = MakeRHIError(RHIErrorCode::eBackendFailure, "RHI submission", __FILE__, __LINE__);
    }

    CollectCompletedBatches();

    return result;
}

RHISubmissionResult RHICommandListExecutor::SubmitCommandLists(std::span<RHICommandList*> lists)
{
    return m_runtime->thread.Invoke(&RHICommandListExecutor::ExecuteBatch, this, lists);
}

RHIQueueCapabilities RHICommandListExecutor::GetQueueCapabilities() const
{
    return m_submissionQueueCapabilities;
}

bool RHICommandListExecutor::PrepareSubmissionDependencies(IRHICommandContext*                      context,
                                                           std::span<const RHISubmissionDependency> dependencies)
{
    return m_runtime->thread.Invoke(&DynamicRHI::PrepareSubmissionDependencies, m_backend, context, dependencies);
}

RHISubmissionTicket RHICommandListExecutor::SubmitFrame(RHICommandList&                     commands,
                                                        RHIViewport*                        viewport,
                                                        RefCountPtr<RHISubmissionState>     state,
                                                        std::span<const RHISubmissionPoint> predecessors)
{
    RHISubmissionGroup group;

    group.commands = &commands;

    group.predecessors.assign(predecessors.begin(), predecessors.end());

    return SubmitFrame(MakeVecView(group), viewport, std::move(state));
}

RHISubmissionTicket RHICommandListExecutor::SubmitFrame(std::span<const RHISubmissionGroup> groups,
                                                        RHIViewport*                        viewport,
                                                        RefCountPtr<RHISubmissionState>     state)
{
    return QueueBatch(groups, viewport, std::move(state), true, true);
}

RHISubmissionTicket RHICommandListExecutor::SubmitQueuedGroups(std::span<const RHISubmissionGroup> groups,
                                                               RefCountPtr<RHISubmissionState>     state)
{
    return QueueBatch(groups, nullptr, std::move(state), false, true);
}

RHIBatchResult RHICommandListExecutor::SubmitGroups(std::span<const RHISubmissionGroup> groups,
                                                    RefCountPtr<RHISubmissionState>     state,
                                                    RHIViewport*                        viewport)
{
    const RHISubmissionTicket ticket = QueueBatch(groups, viewport, std::move(state), false, false);

    RHIBatchResult result;

    if (ticket.IsValid())
    {
        const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();

        result                                            = ticket.Wait();

        m_submitGroupsWaitNS.fetch_add(
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count(),
            std::memory_order_relaxed);
    }
    else if (AreSubmissionsBlocked())
    {
        result.submission = RHISubmissionResult::eFatal;
        result.cause      = GetTerminalError();
    }
    else if (std::this_thread::get_id() == m_renderThread && m_destroyed)
    {
        result.cause = MakeRHIError(RHIErrorCode::eCancelled, "RHI executor shutdown", __FILE__, __LINE__);
    }

    return result;
}

bool RHICommandListExecutor::ValidateGroups(std::span<const RHISubmissionGroup> groups, const RHISubmissionState& state) const
{
    bool valid = state.GetGroupCount() == groups.size();

    for (size_t i = 0; valid && i < groups.size(); ++i)
    {
        const RHISubmissionGroup& group = groups[i];

        valid                           = group.commands != nullptr && group.commands->GetContext() != nullptr;

        if (valid)
        {
            valid = state.Matches(uint32_t(i), group.commands->GetContext()->GetContextType());
        }

        for (size_t j = 0; valid && j < i; ++j)
        {
            valid = group.commands != groups[j].commands;
        }

        for (const RHISubmissionPoint& point : group.predecessors)
        {
            valid &=
                point.IsValid() && (point.state.Get() == &state ? point.group < i : !point.state || point.state->IsQueued());
        }
    }

    return valid;
}

RHISubmissionTicket RHICommandListExecutor::QueueBatch(std::span<const RHISubmissionGroup> groups,
                                                       RHIViewport*                        viewport,
                                                       RefCountPtr<RHISubmissionState>     state,
                                                       bool                                endFrame,
                                                       bool                                asynchronous)
{
    RHISubmissionTicket ticket;

    bool valid = std::this_thread::get_id() == m_renderThread && !m_destroyed && !AreSubmissionsBlocked();

    if (valid && !state)
    {
        HeapVector<RHICommandContextType> queues;

        for (const RHISubmissionGroup& group : groups)
        {
            queues.push_back(group.commands != nullptr && group.commands->GetContext() != nullptr
                                 ? group.commands->GetContext()->GetContextType()
                                 : RHICommandContextType::eMax);
        }

        state = MakeRefCountPtr<RHISubmissionState>(queues);
    }

    valid = valid && ValidateGroups(groups, *state);

    if (valid && state->Queue())
    {
        RefCountPtr<RHICommandBatch> batch = MakeRefCountPtr<RHICommandBatch>();

        batch->submissionState             = state;

        batch->endFrame                    = endFrame;

        batch->asynchronous                = asynchronous;

        batch->viewport                    = viewport;

        batch->resources.Retain(viewport);

        if (viewport != nullptr)
        {
            batch->resources.Retain(viewport->GetColorBackBuffer());

            batch->resources.Retain(viewport->GetDepthStencilBackBuffer());
        }

        for (const RHISubmissionGroup& input : groups)
        {
            RHICommandBatch::Group& group = batch->groups.emplace_back();

            group.predecessors            = input.predecessors;

            group.commands =
                input.commands->DetachCommands(AcquireRecordingCommandList(input.commands->GetContext()->GetContextType()));
        }

        batch->result.groups.resize(groups.size());

        batch->queuedAt = std::chrono::steady_clock::now();

        ticket          = RHISubmissionTicket(batch->completion.get_future().share(), batch->completionEvent, m_runtime);

        ++m_submittedBatches;

        const uint64_t pendingCount = m_pendingBatches.fetch_add(1) + 1;

        m_peakPendingBatches.store(std::max(m_peakPendingBatches.load(), pendingCount));

        m_runtime->thread.Dispatch(std::bind_front(&RHICommandListExecutor::ExecuteFrame, this, batch),
                                   std::bind_front(&RHICommandListExecutor::CancelBatch, this, batch));
    }

    return ticket;
}

bool RHICommandListExecutor::ResolvePredecessors(RHICommandBatch& batch, size_t first, size_t group)
{
    bool ready               = true;

    RHICommandList& commands = *batch.groups[group].commands;

    for (const RHISubmissionPoint& point : batch.groups[group].predecessors)
    {
        // Earlier groups in this same logical-queue run are ordered by the queue and
        // recorded barriers. Every other point must already have native acceptance.
        const bool sameRun = point.state.Get() == batch.submissionState.Get() && point.group >= first && point.group < group
                          && point.queue == commands.GetContext()->GetContextType();

        if (ready && !sameRun)
        {
            RHISubmissionDependency resolved;

            ready = point.Resolve(resolved) == RHISubmissionPointStatus::eAccepted;

            ready = ready && resolved.serial <= m_backend->GetLastSubmittedSerial(resolved.queue);

            if (ready && resolved.queue != commands.GetContext()->GetContextType())
            {
                commands.AddSubmissionDependency(resolved);
            }
        }
    }

    return ready;
}

bool RHICommandListExecutor::ExecuteGroups(RHICommandBatch& batch)
{
    batch.result.submission = RHISubmissionResult::eSuccess;

    bool acceptedWork       = false;

    HeapVector<RHICommandList*> lists;

    size_t first = 0;

    while (batch.result.submission == RHISubmissionResult::eSuccess && first < batch.groups.size())
    {
        const RHICommandContextType queue = batch.groups[first].commands->GetContext()->GetContextType();

        size_t end                        = first + 1;

        // The fence path keeps its existing per-group failure behavior. Timeline
        // submissions atomically accept or reject every batch in this native call.
        // Logical queues can share a native handle but keep separate pending workloads;
        // flush each logical run before advancing so backend queue-drain order cannot reorder it.
        while (m_submissionQueueCapabilities.asyncSubmissionDependencies && end < batch.groups.size()
               && queue == batch.groups[end].commands->GetContext()->GetContextType())
        {
            ++end;
        }

        RHICompletionSet before;

        lists.clear();

        lists.reserve(end - first + 1);

        bool ready = true;

        for (size_t i = first; i < end; ++i)
        {
            RHICommandList* commands                 = batch.groups[i].commands.get();

            const RHICommandContextType logicalQueue = commands->GetContext()->GetContextType();

            before.Extend(logicalQueue, m_backend->GetLastSubmittedSerial(logicalQueue));

            commands->SetSubmissionBoundary(true);

            lists.push_back(commands);

            ready = ready && ResolvePredecessors(batch, first, i);
        }

        // An unresolved predecessor is fatal. Missing presentation storage submits nothing
        // and stays retryable unless earlier work in this batch was already accepted.
        RHISubmissionResult unsubmitted = RHISubmissionResult::eFatal;

        // The frame's last graphics group carries the presentation copy in the same native
        // submission. The copy keeps its own command buffer, so only the copy waits for the
        // acquired image.
        if (ready && batch.viewport != nullptr && end == batch.groups.size()
            && batch.groups[end - 1].commands->GetContext()->GetContextType() == RHICommandContextType::eGraphics)
        {
            RHIStatus             prepared{};
            RHICommandList* const present = PreparePresentCopy(batch, prepared);

            ready                         = present != nullptr && static_cast<bool>(prepared);

            if (present != nullptr)
            {
                lists.push_back(present);
            }
            else
            {
                unsubmitted = RHISubmissionResult::eRejected;
            }
        }

        RHISubmissionResult submitted = ready ? ExecuteBatch(lists) : unsubmitted;

        const RHIError cause =
            ready ? m_executionError
                  : RHIErrorOr(batch.result.cause,
                               MakeRHIError(RHIErrorCode::eInvalidArgument, "RHI group preparation", __FILE__, __LINE__));

        if (!batch.result.cause.IsFailure() && cause.IsFailure())
        {
            batch.result.cause = cause;
        }

        for (size_t i = first; i < end; ++i)
        {
            const RHICommandList& commands    = *batch.groups[i].commands;

            RHISubmissionGroupResult& result  = batch.result.groups[i];

            result.accepted                   = {commands.GetContext()->GetContextType(), commands.GetSubmittedSerial()};

            acceptedWork                     |= result.accepted.serial > before.Get(result.accepted.queue);
        }

        if (submitted != RHISubmissionResult::eSuccess && acceptedWork)
        {
            submitted = RHISubmissionResult::eFatal;
        }

        for (size_t i = first; i < end; ++i)
        {
            RHISubmissionGroupResult& result = batch.result.groups[i];

            result.cause                     = cause;

            result.submission                = submitted;

            if (result.submission == RHISubmissionResult::eSuccess
                && !batch.submissionState->Accept(uint32_t(i), result.accepted))
            {
                submitted = result.submission = RHISubmissionResult::eFatal;
            }
        }

        batch.result.submission = submitted;

        first                   = end;
    }

    return acceptedWork;
}

void RHICommandListExecutor::CancelBatch(const RefCountPtr<RHICommandBatch>& batch)
{
    if (!batch->executionFinished)
    {
        m_blocked.store(true, std::memory_order_release);

        batch->submissionState->Fail();

        batch->result.submission = RHISubmissionResult::eFatal;

        batch->result.cause      = GetTerminalError();

        if (!batch->result.cause.IsFailure())
        {
            batch->result.cause = MakeRHIError(RHIErrorCode::eCancelled, "RHI job cancelled", __FILE__, __LINE__);
        }


        PublishTerminalError(batch->result.cause);

        batch->executionFinished = true;

        ++m_completedBatches;

        --m_pendingBatches;

        batch->completion.set_value(batch->result);

        batch->completionEvent->Signal();
    }
}

RHICommandList* RHICommandListExecutor::PreparePresentCopy(RHICommandBatch& batch, RHIStatus& prepared)
{
    batch.presentCommands = AcquirePresentCommandList();

    prepared = batch.presentCommands != nullptr ? batch.viewport->PrepareForPresent(batch.presentCommands.get()) : RHIStatus{};

    if (!prepared)
    {
        batch.result.cause = prepared.error;
    }

    return batch.presentCommands.get();
}

void RHICommandListExecutor::PresentFrame(RHICommandBatch& batch, bool acceptedWork)
{
    RHIBatchResult& result = batch.result;

    // A frame whose last group is not graphics presents in its own submission.
    if (batch.presentCommands == nullptr)
    {
        RHIStatus             prepared{};
        RHICommandList* const present = PreparePresentCopy(batch, prepared);

        if (present != nullptr)
        {
            RHICommandList* lists[] = {present};
            const uint64_t  before  = m_backend->GetLastSubmittedSerial(RHICommandContextType::eGraphics);

            result.submission       = prepared ? ExecuteBatch(lists) : RHISubmissionResult::eFatal;

            if (!result.cause.IsFailure())
            {
                result.cause = m_executionError;
            }

            if (result.submission == RHISubmissionResult::eRejected
                && (acceptedWork || m_backend->GetLastSubmittedSerial(RHICommandContextType::eGraphics) > before))
            {
                result.submission = RHISubmissionResult::eFatal;
            }
        }
        else
        {
            result.submission = acceptedWork ? RHISubmissionResult::eFatal : RHISubmissionResult::eRejected;
        }
    }

    if (result.submission == RHISubmissionResult::eSuccess)
    {
        result.presentation = batch.viewport->Present();

        if (result.presentation.error.IsFailure())
        {
            result.cause = result.presentation.error;

            if (result.presentation.acceptance != RHIPresentAcceptance::eRejected
                || result.cause.code == RHIErrorCode::eDeviceLost)
            {
                result.submission = RHISubmissionResult::eFatal;
            }
        }

        result.needsRecreation = batch.viewport->NeedsRecreation();
    }
}

void RHICommandListExecutor::FinishFrame(RHICommandBatch& batch, const RHICompletionSet& submittedBefore)
{
    RHIBatchResult& result = batch.result;

    if (result.submission != RHISubmissionResult::eSuccess && !result.cause.IsFailure())
    {
        result.cause = MakeRHIError(RHIErrorCode::eBackendFailure, "RHI frame submission", __FILE__, __LINE__);
    }

    // Once accepted asynchronously, rejection invalidates dependent scheduled state.
    // Never execute later batches against state that did not reach the GPU.
    if (result.submission != RHISubmissionResult::eSuccess)
    {
        batch.submissionState->Fail();

        // SubmitGroups waits for this result before publishing state, even with a viewport.
        // Only an asynchronous handoff or possible partial execution makes rejection terminal.
        if (batch.asynchronous || result.submission == RHISubmissionResult::eFatal)
        {
            PublishTerminalError(result.cause);
        }
    }

    PublishProgress();

    if (AreSubmissionsBlocked() && result.submission == RHISubmissionResult::eSuccess)
    {
        result.submission = RHISubmissionResult::eFatal;

        result.cause      = GetTerminalError();
    }

    // Every native flush publishes progress, so the cached serials bracket this batch's own work.
    const RHICompletionSet submittedAfter = GetCachedSubmittedSerials();

    for (size_t i = 0; i < RHICompletionSet::kQueueCount; ++i)
    {
        if (submittedAfter.serials[i] > submittedBefore.serials[i])
        {
            result.requiredSerials.serials[i] = submittedAfter.serials[i];
        }
    }

    if (result.submission == RHISubmissionResult::eSuccess && !batch.submissionState->FinishSubmission())
    {
        result.submission = RHISubmissionResult::eFatal;

        result.cause =
            MakeRHIError(RHIErrorCode::eBackendFailure, "Incomplete scheduled producer submission", __FILE__, __LINE__);

        PublishTerminalError(result.cause);
    }

    if (result.submission != RHISubmissionResult::eSuccess)
    {
        batch.submissionState->Fail();
    }
}

void RHICommandListExecutor::ExecuteFrame(const RefCountPtr<RHICommandBatch>& batch)
{
    m_runtime->thread.CheckOwnership();

    const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();

    const double queueWaitUs               = std::chrono::duration<double, std::micro>(start - batch->queuedAt).count();

    const RHICompletionSet submittedBefore = GetCachedSubmittedSerials();

    const bool beganExecution              = !AreSubmissionsBlocked();

    if (beganExecution)
    {
        // Reserve retirement ownership before native execution can fail.
        m_retired.push_back(batch);

        const bool acceptedWork = ExecuteGroups(*batch);

        if (batch->result.submission == RHISubmissionResult::eSuccess && batch->viewport != nullptr)
        {
            PresentFrame(*batch, acceptedWork);
        }

        if (batch->result.submission == RHISubmissionResult::eSuccess && batch->endFrame)
        {
            m_backend->EndFrame();
        }
    }
    else
    {
        batch->result.submission = RHISubmissionResult::eFatal;

        batch->result.cause      = GetTerminalError();
    }

    FinishFrame(*batch, submittedBefore);

    const double executionCPUUs = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count();

    m_executionCPUUs.fetch_add(static_cast<uint64_t>(executionCPUUs));

    m_queueWaitUs.fetch_add(static_cast<uint64_t>(queueWaitUs));

    ++m_completedBatches;

    --m_pendingBatches;

    batch->executionFinished = true;

    batch->completion.set_value(batch->result);

    batch->completionEvent->Signal();

    if (beganExecution)
    {
        CollectCompletedBatches();
    }
}

void RHICommandListExecutor::ExecuteBeginFrame()
{
    if (!AreSubmissionsBlocked())
    {
        m_backend->BeginFrame();

        PublishProgress();

        CollectCompletedBatches();
    }
}

void RHICommandListExecutor::BeginFrame()
{
    m_runtime->thread.Dispatch(std::bind_front(&RHICommandListExecutor::ExecuteBeginFrame, this));
}

void RHICommandListExecutor::EndFrame()
{
    m_runtime->thread.Invoke(&DynamicRHI::EndFrame, m_backend);
}

void RHICommandListExecutor::BeginGPUFrameTiming(const RHIGPUFrameTimingPtr& timing)
{
    m_runtime->thread.Dispatch(std::bind_front(&DynamicRHI::BeginGPUFrameTiming, m_backend, timing), [timing] {
        if (timing)
        {
            timing->Seal(RHIGPUTimingStatus::eError);
        }
    });
}

void RHICommandListExecutor::EndGPUFrameTiming(const RHIGPUFrameTimingPtr& timing, bool succeeded)
{
    if (!m_runtime->thread.DispatchCleanup(
            std::bind_front(&RHICommandListExecutor::ExecuteEndGPUFrameTiming, this, timing, succeeded))
        && timing)
    {
        timing->Seal(RHIGPUTimingStatus::eError);
    }
}

void RHICommandListExecutor::ExecuteEndGPUFrameTiming(const RHIGPUFrameTimingPtr& timing, bool succeeded)
{
    // Include failures discovered asynchronously after render-thread handoff.
    m_backend->EndGPUFrameTiming(timing, succeeded && !AreSubmissionsBlocked());
}

void RHICommandListExecutor::ExecuteWaitIdle()
{
    m_backend->WaitDeviceIdle();

    PublishProgress();

    CollectCompletedBatches();
}

void RHICommandListExecutor::WaitDeviceIdle()
{
    m_runtime->thread.Invoke(&RHICommandListExecutor::ExecuteWaitIdle, this);
}

bool RHICommandListExecutor::ExecuteWaitForCompletion(RHICommandContextType type, uint64_t serial, uint64_t timeoutNS)
{
    const bool completed = static_cast<bool>(m_backend->WaitForCompletion(type, serial, timeoutNS));

    PublishProgress();

    CollectCompletedBatches(false);

    return completed;
}

RHIWaitResult RHICommandListExecutor::WaitForCompletion(RHICommandContextType type, uint64_t serial, uint64_t timeoutNS)
{
    const bool completed =
        serial <= GetCachedCompletedSerials().Get(type)
        || m_runtime->thread.Invoke(&RHICommandListExecutor::ExecuteWaitForCompletion, this, type, serial, timeoutNS);

    return MakeWaitResult(completed);
}

uint64_t RHICommandListExecutor::GetLastSubmittedSerial(RHICommandContextType type) const
{
    return m_mode == RHIExecutionMode::eInline
             ? m_backend->GetLastSubmittedSerial(type)
             : m_queueProgress[static_cast<uint32_t>(type)]->submitted.load(std::memory_order_acquire);
}

uint64_t RHICommandListExecutor::QueryLastCompletedSerial(RHICommandContextType type)
{
    uint64_t completed = m_queueProgress[static_cast<uint32_t>(type)]->completed.load(std::memory_order_acquire);

    if (m_mode == RHIExecutionMode::eInline)
    {
        completed = m_backend->QueryLastCompletedSerial(type);

        PublishSubmissionStatus();
    }

    return completed;
}

void RHICommandListExecutor::RefreshGPUProgress()
{
    PublishProgress();

    CollectCompletedBatches();

    // Releasing command arenas can release the last recording of a bindless epoch.
    m_backend->CollectRetiredBindlessResources();

    PublishSubmissionStatus();
}

void RHICommandListExecutor::ExecutePollGPUProgress()
{
    RefreshGPUProgress();

    m_progressPollPending.store(false, std::memory_order_release);
}

void RHICommandListExecutor::PollGPUProgress()
{
    bool pending = false;

    if (m_progressPollPending.compare_exchange_strong(pending, true, std::memory_order_acq_rel))
    {
        if (!m_runtime->thread.TryDispatch(std::bind_front(&RHICommandListExecutor::ExecutePollGPUProgress, this)))
        {
            m_progressPollPending.store(false, std::memory_order_release);
        }
    }
}

void RHICommandListExecutor::FlushRHIThread()
{
    // The ordered invocation fences CPU work and samples GPU progress without waiting
    // for unfinished GPU submissions or changing the CPU-only flush contract.
    m_runtime->thread.Invoke(&RHICommandListExecutor::RefreshGPUProgress, this);
}

bool RHICommandListExecutor::AreSubmissionsBlocked() const
{
    return m_blocked.load(std::memory_order_acquire) || m_runtime->thread.HasTaskFailure();
}

RHIThreadMetrics RHICommandListExecutor::GetThreadMetrics() const
{
    return {m_submittedBatches.load(),   m_completedBatches.load(),         m_executionCPUUs.load(),
            m_queueWaitUs.load(),        m_pendingBatches.load(),           m_peakPendingBatches.load(),
            m_submitGroupsWaitNS.load(), m_backend->GetExecutionCounters(), RHIWorkerTimer::Read()};
}

RHIAPIType RHICommandListExecutor::GetAPIType()
{
    return m_apiType;
}

std::string_view RHICommandListExecutor::GetName()
{
    return m_name;
}

DataFormat RHICommandListExecutor::GetSupportedDepthFormat()
{
    return m_depthFormat;
}

const RHIDeviceDesc& RHICommandListExecutor::GetDeviceDesc() const
{
    return m_backend->GetDeviceDesc();
}

uint64_t RHICommandListExecutor::GetNativeObject(RHINativeObject object) const
{
    return m_backend->GetNativeObject(object);
}

const RHIGPUInfo& RHICommandListExecutor::QueryGPUInfo() const
{
    return m_mode == RHIExecutionMode::eInline ? m_backend->QueryGPUInfo() : m_gpuInfo;
}

RHIGPUMemoryStats RHICommandListExecutor::GetGPUMemoryStats() const
{
    // This backend API explicitly permits concurrent reads; do not enqueue an RHI wait.
    return m_backend->GetGPUMemoryStats();
}

RHIQueueCopyCapabilities RHICommandListExecutor::GetQueueCopyCapabilities(RHICommandContextType type) const
{
    return m_mode == RHIExecutionMode::eInline ? m_backend->GetQueueCopyCapabilities(type)
                                               : m_queueCapabilities[static_cast<uint32_t>(type)];
}

void RHICommandListExecutor::DestroyViewport(RHIViewport* viewport)
{
    ZEN_ASSERT(std::this_thread::get_id() == m_renderThread);

    // The window owner drives surface teardown, and the backend finishes with the native
    // window before returning. Other resources reach RHI cleanup through ReleaseReference.
    m_backend->DestroyViewport(viewport);
}

RHIViewport* RHICommandListExecutor::CreateViewport(zen::RHIWindowHandle window, uint32_t width, uint32_t height, bool vsync)
{
    ZEN_ASSERT(std::this_thread::get_id() == m_renderThread);

    return AreSubmissionsBlocked() ? nullptr : m_backend->CreateViewport(window, width, height, vsync);
}

RHIShader* RHICommandListExecutor::CreateShader(const RHIShaderCreateInfo& info)
{
    return InvokeOrDefault<RHIShader*>(m_runtime->thread, &DynamicRHI::CreateShader, m_backend, std::cref(info));
}

RHIPipeline* RHICommandListExecutor::CreatePipeline(const RHIComputePipelineCreateInfo& info)
{
    return InvokeOrDefault<RHIPipeline*>(m_runtime->thread, [&] { return m_backend->CreatePipeline(info); });
}

RHIPipeline* RHICommandListExecutor::CreatePipeline(const RHIGfxPipelineCreateInfo& info)
{
    return InvokeOrDefault<RHIPipeline*>(m_runtime->thread, [&] { return m_backend->CreatePipeline(info); });
}

RHIPipeline* RHICommandListExecutor::CreatePipeline(const RHIRayTracingPipelineCreateInfo& info)
{
    return InvokeOrDefault<RHIPipeline*>(m_runtime->thread, [&] { return m_backend->CreatePipeline(info); });
}

RHIError RHICommandListExecutor::SnapshotPipelineCache(size_t byteLimit, std::vector<uint8_t>& bytes)
{
    RHIError error = GetTerminalError();
    if (!error.IsFailure())
    {
        // RHIError itself cannot be the value type of RHIResult. Keep it in caller storage until the ordered job finishes.
        const RHIResult<void> call =
            m_runtime->thread.InvokeChecked([&] { error = m_backend->SnapshotPipelineCache(byteLimit, bytes); });
        if (!call)
        {
            error = call.GetError();
        }
    }
    return error;
}

RHISampler* RHICommandListExecutor::CreateSampler(const RHISamplerCreateInfo& info)
{
    return InvokeOrDefault<RHISampler*>(m_runtime->thread, &DynamicRHI::CreateSampler, m_backend, std::cref(info));
}

RHIBindlessHandle RHICommandListExecutor::RegisterBindlessResource(RHIResource* resource, uint32_t slot)
{
    return InvokeOrDefault<RHIBindlessHandle>(m_runtime->thread, &DynamicRHI::RegisterBindlessResource, m_backend, resource,
                                              slot);
}

bool RHICommandListExecutor::UnregisterBindlessResource(RHIBindlessHandle handle)
{
    return m_runtime->thread.Invoke(&DynamicRHI::UnregisterBindlessResource, m_backend, handle);
}

bool RHICommandListExecutor::IsBindlessResourceRegistered(RHIBindlessHandle handle)
{
    return m_runtime->thread.Invoke(&DynamicRHI::IsBindlessResourceRegistered, m_backend, handle);
}

bool RHICommandListExecutor::ResetBindlessResources()
{
    const bool reset = m_runtime->thread.Invoke(&DynamicRHI::ResetBindlessResources, m_backend);

    return reset;
}

void RHICommandListExecutor::CollectRetiredBindlessResources()
{
    FlushRHIThread();
}

RHITexture* RHICommandListExecutor::CreateTexture(const RHITextureCreateInfo& info)
{
    return InvokeOrDefault<RHITexture*>(m_runtime->thread, &DynamicRHI::CreateTexture, m_backend, std::cref(info));
}

RHITextureView* RHICommandListExecutor::CreateTextureView(RHITexture* texture, const RHITextureViewCreateInfo& info)
{
    return InvokeOrDefault<RHITextureView*>(m_runtime->thread, &DynamicRHI::CreateTextureView, m_backend, texture,
                                            std::cref(info));
}

RHIAccelerationStructureBuildSizes RHICommandListExecutor::GetAccelerationStructureBuildSizes(
    const RHIAccelerationStructureBuildDesc& desc)
{
    return InvokeOrDefault<RHIAccelerationStructureBuildSizes>(
        m_runtime->thread, &DynamicRHI::GetAccelerationStructureBuildSizes, m_backend, std::cref(desc));
}

RHIAccelerationStructure* RHICommandListExecutor::CreateAccelerationStructure(const RHIAccelerationStructureCreateInfo& info)
{
    return InvokeOrDefault<RHIAccelerationStructure*>(m_runtime->thread, &DynamicRHI::CreateAccelerationStructure, m_backend,
                                                      std::cref(info));
}

RHIBuffer* RHICommandListExecutor::CreateBuffer(const RHIBufferCreateInfo& info)
{
    return InvokeOrDefault<RHIBuffer*>(m_runtime->thread, &DynamicRHI::CreateBuffer, m_backend, std::cref(info));
}

uint64_t RHICommandListExecutor::GetBufferAllocationSize(const RHIBufferCreateInfo& info)
{
    return InvokeOrDefault<uint64_t>(m_runtime->thread, &DynamicRHI::GetBufferAllocationSize, m_backend, std::cref(info));
}

IRHICommandContext* RHICommandListExecutor::GetCommandContext(RHICommandContextType type)
{
    return InvokeOrDefault<IRHICommandContext*>(m_runtime->thread, &DynamicRHI::GetCommandContext, m_backend, type);
}

RHITextureCopyCapabilities RHICommandListExecutor::GetTextureCopyCapabilities(DataFormat format) const
{
    return m_backend->GetTextureCopyCapabilities(format);
}
} // namespace zen
