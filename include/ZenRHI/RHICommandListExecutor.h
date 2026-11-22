#pragma once
#include <span>
#include <mutex>
#include "DynamicRHI.h"
#include "RHIThread.h"
#include "ZenFoundation/Templates/SmallVector.h"
#include "ZenFoundation/Utils/UniquePtr.h"
#include <atomic>
#include <chrono>
#include <future>
#include "ZenFoundation/Utils/RefCountPtr.h"

namespace zen
{
enum class RHISubmissionPointStatus : uint8_t
{
    eInvalid,
    ePending,
    eAccepted,
    eFailed
};

// Owned native-acceptance results. Queue assignments never change after construction.
// RenderCore can retain a point while the ordered RHI worker fills its exact serial.
class RHISubmissionState final : public RefCounted
{
public:
    explicit RHISubmissionState(std::span<const RHICommandContextType> queues);

    bool IsQueued() const;

    bool Matches(uint32_t group, RHICommandContextType queue) const;

    size_t GetGroupCount() const;

    // Successful CPU processing of every group; does not establish GPU completion.
    bool IsSubmissionFinished() const;

    RHISubmissionPointStatus Resolve(uint32_t group, RHISubmissionDependency& submission) const;

private:
    friend class RHICommandListExecutor;
    friend struct RHISubmissionStateTestAccess;

    // Only the executor publishes handoff, native acceptance, and final status.
    bool Queue();

    bool Accept(uint32_t group, RHISubmissionDependency submission);

    bool FinishSubmission();

    void Fail();

    struct Group
    {
        RHISubmissionDependency submission;
        bool                    accepted{false};
    };
    HeapVector<Group>  m_groups;
    mutable std::mutex m_mutex;
    bool               m_queued{false};
    bool               m_failed{false};
    bool               m_submissionFinished{false};
};

using RHISubmissionStatePtr = RefCountPtr<RHISubmissionState>;

struct RHISubmissionPoint
{
    RHICommandContextType queue{RHICommandContextType::eGraphics};
    uint64_t              serial{0};
    RHISubmissionStatePtr state;
    uint32_t              group{UINT32_MAX};

    bool IsValid() const;

    bool operator==(const RHISubmissionPoint& other) const;

    RHISubmissionPointStatus Resolve(RHISubmissionDependency& submission) const;
};

// Borrowed recording inputs. Handoff validates the complete schedule before detaching any list.
struct RHISubmissionGroup
{
    RHICommandList*                 commands{nullptr};
    std::vector<RHISubmissionPoint> predecessors;
};

struct RHISubmissionGroupResult
{
    RHIError                cause{};
    RHISubmissionResult     submission{RHISubmissionResult::eRejected};
    RHISubmissionDependency accepted;
};

struct RHIBatchResult
{
    RHIError            cause{};
    RHIPresentResult    presentation{};
    RHISubmissionResult submission{RHISubmissionResult::eRejected};
    // Native submissions made by this batch, including partial execution and presentation.
    RHICompletionSet                      requiredSerials;
    std::vector<RHISubmissionGroupResult> groups;
    bool                                  presented{false};
    bool                                  needsRecreation{false};
    double                                executionCPUUs{0};
    double                                queueWaitUs{0};
    std::string                           error;
};

class RHISubmissionTicket
{
public:
    RHISubmissionTicket() = default;

    RHISubmissionTicket(std::shared_future<RHIBatchResult> result, RHIThreadEventPtr completion);

    bool IsValid() const;

    bool IsReady() const;

    // Waits for CPU submission processing, not GPU completion. The result is
    // borrowed from this ticket; keep a ticket alive while using the reference.
    const RHIBatchResult& Wait() const;

    bool operator==(const RHISubmissionTicket& other) const
    {
        return m_completion.Get() == other.m_completion.Get();
    }

private:
    std::shared_future<RHIBatchResult> m_result;
    RHIThreadEventPtr                  m_completion;
};

struct RHICommandBatch;

struct RHIThreadMetrics
{
    uint64_t             submittedBatches{0};
    uint64_t             completedBatches{0};
    uint64_t             executionCPUUs{0};
    uint64_t             queueWaitUs{0};
    uint64_t             pendingBatches{0};
    uint64_t             peakPendingBatches{0};
    uint64_t             submitGroupsWaitNS{0};
    RHIExecutionCounters native;
};

// DynamicRHI facade: RenderCore keeps its API, while mutable backend operations
// are marshalled to one executor. Raw backend tests can still run without it.
class RHICommandListExecutor final : public DynamicRHI
{
public:
    RHICommandListExecutor(DynamicRHI* backend, RHIExecutionMode mode);

    ~RHICommandListExecutor() override;

    RHICommandListExecutor(const RHICommandListExecutor&)            = delete;

    RHICommandListExecutor& operator=(const RHICommandListExecutor&) = delete;

    RHISubmissionTicket SubmitFrame(RHICommandList&                     commands,
                                    RHIViewport*                        viewport,
                                    RHISubmissionStatePtr               state        = {},
                                    std::span<const RHISubmissionPoint> predecessors = {});

    RHISubmissionTicket SubmitFrame(std::span<const RHISubmissionGroup> groups,
                                    RHIViewport*                        viewport,
                                    RHISubmissionStatePtr               state = {});

    // Ordered handoff without presentation or EndFrame. Late rejection blocks dependent batches.
    RHISubmissionTicket SubmitQueuedGroups(std::span<const RHISubmissionGroup> groups, RHISubmissionStatePtr state);

    // Synchronous CPU handoff for standalone graphs; does not end the native frame. A viewport
    // is presented after the groups, as SubmitFrame does. Rejection before any acceptance is
    // retryable because the caller has not published scheduled state. SubmitFrame rejection blocks.
    RHIBatchResult SubmitGroups(std::span<const RHISubmissionGroup> groups,
                                RHISubmissionStatePtr               state,
                                RHIViewport*                        viewport = nullptr);

    RHISubmissionResult SubmitBatch(std::span<RHICommandList*> lists);

    // Request one coalesced, nonblocking GPU progress/retirement sweep on RHI.
    // Cached getters do not refresh progress; poll again if work is still in flight.
    void PollGPUProgress();

    // Transfer a reference after submitting the last batch that uses the resource.
    // Collection releases it after every previously submitted queue has completed.
    void ReleaseWhenComplete(RHIResourcePtr<RHIResource> resource);
    void CollectGarbage();

    // Published values only, in both execution modes. No backend calls or worker waits.
    RHICompletionSet GetCachedSubmittedSerials() const;

    RHICompletionSet GetCachedCompletedSerials() const;

    void FlushRHIThread();

    bool AreSubmissionsBlocked() const override;

    RHIError GetTerminalError() const override;

    bool HasDeviceLoss() const override
    {
        return GetRHIThread().HasDeviceLoss();
    }

    RHIThreadMetrics GetThreadMetrics() const;

    void NotifyResourceDestroyed(uint64_t resourceId) override;

    void DrainDestroyedResourceIds(std::vector<uint64_t>& resourceIds);

    void Destroy() override;

    void BeginFrame() override;

    void BeginGPUFrameTiming(const RHIGPUFrameTimingPtr& timing) override;

    void EndGPUFrameTiming(const RHIGPUFrameTimingPtr& timing, bool succeeded) override;

    void EndFrame() override;

    RHISubmissionResult FlushAllGPUCommands() override;

    void WaitDeviceIdle() override;

    bool WaitForCompletion(RHICommandContextType type, uint64_t serial, uint64_t timeoutNS = UINT64_MAX) override;

    uint64_t GetLastSubmittedSerial(RHICommandContextType type) const override;

    // Polls the backend inline; threaded mode reads published progress without a worker wait.
    uint64_t QueryLastCompletedSerial(RHICommandContextType type) override;

    RHIAPIType GetAPIType() override;

    std::string_view GetName() override;

    DataFormat GetSupportedDepthFormat() override;

    RHIQueueCapabilities GetQueueCapabilities() const override;

    bool PrepareSubmissionDependencies(IRHICommandContext*                      context,
                                       std::span<const RHISubmissionDependency> dependencies) override;

    const RHIGPUInfo& QueryGPUInfo() const override;

    RHIGPUMemoryStats GetGPUMemoryStats() const override;

    RHIQueueCopyCapabilities GetQueueCopyCapabilities(RHICommandContextType type) const override;

    void Init() override;

    void DestroyViewport(RHIViewport* viewport) override;

    RHIViewport* CreateViewport(zen::RHIWindowHandle window, uint32_t width, uint32_t height, bool vsync) override;

    RHIShader* CreateShader(const RHIShaderCreateInfo& info) override;

    void DestroyShader(RHIShader* shader) override;

    RHIPipeline* CreatePipeline(const RHIComputePipelineCreateInfo& info) override;

    RHIPipeline* CreatePipeline(const RHIRayTracingPipelineCreateInfo& info) override;

    RHIPipeline* CreatePipeline(const RHIGfxPipelineCreateInfo& info) override;

    void DestroyPipeline(RHIPipeline* pipeline) override;

    RHISampler* CreateSampler(const RHISamplerCreateInfo& info) override;

    void DestroySampler(RHISampler* sampler) override;

    RHIBindlessHandle RegisterBindlessResource(RHIResource* resource, uint32_t slot) override;

    bool UnregisterBindlessResource(RHIBindlessHandle handle) override;

    bool IsBindlessResourceRegistered(RHIBindlessHandle handle) override;

    void CollectRetiredBindlessResources() override;

    bool ResetBindlessResources() override;

    RHITexture* CreateTexture(const RHITextureCreateInfo& info) override;

    RHITextureView* CreateTextureView(RHITexture* texture, const RHITextureViewCreateInfo& info) override;

    void DestroyTexture(RHITexture* texture) override;

    RHIAccelerationStructureBuildSizes GetAccelerationStructureBuildSizes(
        const RHIAccelerationStructureBuildDesc& desc) override;

    RHIAccelerationStructure* CreateAccelerationStructure(const RHIAccelerationStructureCreateInfo& info) override;

    void DestroyAccelerationStructure(RHIAccelerationStructure* structure) override;

    RHIBuffer* CreateBuffer(const RHIBufferCreateInfo& info) override;

    uint64_t GetBufferAllocationSize(const RHIBufferCreateInfo& info) override;

    void DestroyBuffer(RHIBuffer* buffer) override;

    IRHICommandContext* GetCommandContext(RHICommandContextType type) override;

    IRHICommandContext* GetTransferCommandContext() override;

    RHIStatus FinalizeCommandLists(std::span<RHICommandList*> lists, std::vector<RHIPlatformCommandList*>& output) override;

    void SubmitPlatformCommandLists(std::span<RHIPlatformCommandList*> lists) override;

    RHITextureCopyCapabilities GetTextureCopyCapabilities(DataFormat format) const override;

private:
    RHISubmissionTicket QueueBatch(std::span<const RHISubmissionGroup> groups,
                                   RHIViewport*                        viewport,
                                   RHISubmissionStatePtr               state,
                                   bool                                endFrame,
                                   bool                                asynchronous);

    bool ValidateGroups(std::span<const RHISubmissionGroup> groups, const RHISubmissionState& state) const;

    bool ResolvePredecessors(RHICommandBatch& batch, size_t first, size_t group);

    bool ExecuteGroups(RHICommandBatch& batch);
    struct QueueProgress
    {
        std::atomic<uint64_t> submitted{0};
        std::atomic<uint64_t> completed{0};
    };

    void ExecuteFrame(const RefCountPtr<RHICommandBatch>& batch);

    void CancelBatch(const RefCountPtr<RHICommandBatch>& batch);

    RHISubmissionResult ExecuteBatch(std::span<RHICommandList*> lists);

    void ExecuteBeginFrame();

    void ExecuteEndGPUFrameTiming(const RHIGPUFrameTimingPtr& timing, bool succeeded);

    bool ExecuteWaitForCompletion(RHICommandContextType type, uint64_t serial, uint64_t timeoutNS);

    void ExecuteWaitIdle();

    void ExecuteDestroy();

    void DeleteBackend();

    void PublishProgress();

    void PublishSubmissionStatus();

    void PublishTerminalError(RHIError error);

    void RefreshGPUProgress();

    void ExecutePollGPUProgress();

    struct DeferredResource
    {
        RHICompletionSet            serials;
        RHIResourcePtr<RHIResource> resource;
    };
    HeapVector<DeferredResource> m_deferredResources;
    void                         RetainUntilComplete(RHIResourcePtr<RHIResource> resource);
    void                         CollectCompletedBatches(bool force = false);

    RHICommandListPtr AcquireRecordingCommandList(RHICommandContextType queue);

    RHICommandListPtr AcquirePresentCommandList();

    void RecycleCommandLists(RHICommandBatch& batch);

    DynamicRHI*                                                          m_backend;
    RHIExecutionMode                                                     m_mode;
    std::thread::id                                                      m_renderThread;
    std::atomic<bool>                                                    m_blocked{false};
    mutable std::mutex                                                   m_errorMutex;
    RHIError                                                             m_terminalError{};
    RHIError                                                             m_executionError{};
    std::atomic<bool>                                                    m_progressPollPending{false};
    bool                                                                 m_destroyed{false};
    RHIGPUInfo                                                           m_gpuInfo;
    RHIAPIType                                                           m_apiType;
    NameID                                                               m_name;
    DataFormat                                                           m_depthFormat;
    const RHIQueueCapabilities                                           m_submissionQueueCapabilities;
    SmallVector<RHIQueueCopyCapabilities, RHICompletionSet::kQueueCount> m_queueCapabilities;
    // Populate before starting the worker; owned atomics retain stable addresses.
    SmallVector<UniquePtr<QueueProgress>, RHICompletionSet::kQueueCount> m_queueProgress;
    HeapVector<RefCountPtr<RHICommandBatch>>                             m_retired;
    // Limit idle storage after a burst. In-flight batches always retain their own lists.
    static constexpr size_t kMaxRecycledCommandLists = RHIFrameState::kMaxFramesInFlight;
    std::mutex              m_recordingCommandListMutex;
    SmallVector<HeapVector<RHICommandListPtr>, RHICompletionSet::kQueueCount> m_recordingCommandLists =
        SmallVector<HeapVector<RHICommandListPtr>, RHICompletionSet::kQueueCount>(RHICompletionSet::kQueueCount);
    // Presentation lists and their contexts stay on RHI until executor destruction.
    HeapVector<RHICommandListPtr> m_presentCommandLists;
    std::mutex                    m_destroyedResourceMutex;
    std::vector<uint64_t>         m_destroyedResourceIds;
    std::atomic<uint64_t>         m_submittedBatches{0};
    std::atomic<uint64_t>         m_completedBatches{0};
    std::atomic<uint64_t>         m_executionCPUUs{0};
    std::atomic<uint64_t>         m_queueWaitUs{0};
    std::atomic<uint64_t>         m_pendingBatches{0};
    std::atomic<uint64_t>         m_peakPendingBatches{0};
    std::atomic<uint64_t>         m_submitGroupsWaitNS{0};
};
} // namespace zen
