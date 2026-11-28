#pragma once
#include <span>
#include "RHICommandList.h"
#include "RHICommon.h"
#include "RHIDeviceDesc.h"
#include "RHIDeviceRuntime.h"
#include "RHIResource.h"
#include "RHIFrameState.h"
#include "RHIGPUFrameTiming.h"
#include "RHIGPUMemoryStats.h"
#include "RHIExecutionCounters.h"

namespace zen
{
class DynamicRHI;
struct RHIAdapterInfo
{
    std::string name;
    uint32_t    vendorID{};
    uint32_t    deviceID{};
    uint32_t    driverVersion{};
    uint32_t    apiVersion{};
    std::string unsupportedReason;
};

// Startup calls are externally serialized. Enumeration needs no logical device. One backend is live per process:
// while one is, both calls fail with eInvalidArgument and leave it untouched.
RHIResult<std::vector<RHIAdapterInfo>> EnumerateRHIAdapters(const RHIDeviceDesc& description);
RHIResult<DynamicRHI*>                 CreateRHIChecked(const RHIDeviceDesc& description);
DynamicRHI*                            CreateRHI(const RHIDeviceDesc& description);

enum class RHINativeObject : uint8_t
{
    eInstance,
    ePhysicalDevice,
    eDevice
};

class DynamicRHI
{
public:
    virtual ~DynamicRHI()  = default;

    virtual void Destroy() = 0;

    virtual RHIExecutionCounters GetExecutionCounters() const
    {
        return {};
    }

    virtual void BeginFrame() = 0;

    // Scope native graphics/compute command buffers without changing submission order.
    // The executor marshals these markers onto the ordered RHI worker.
    virtual void BeginGPUFrameTiming(const RHIGPUFrameTimingPtr& timing)
    {
        if (timing)
        {
            timing->Seal(RHIGPUTimingStatus::eUnsupported);
        }
    }

    virtual void EndGPUFrameTiming(const RHIGPUFrameTimingPtr& timing, bool succeeded) {}

    virtual void EndFrame()
    {
        GetRuntime().frameState.Advance();
    }

    RHICompletionSet GetSubmittedSerials() const
    {
        RHICompletionSet result;
        for (size_t i = 0; i < RHICompletionSet::kQueueCount; ++i)
        {
            const RHICommandContextType queue = static_cast<RHICommandContextType>(i);
            result.Extend(queue, GetLastSubmittedSerial(queue));
        }
        return result;
    }

    // Uses the facade's query policy for each queue; does not wait for GPU completion.
    // Raw backends poll; the executor polls inline or reads published progress when threaded.
    RHICompletionSet QueryCompletedSerials()
    {
        RHICompletionSet result;
        for (size_t i = 0; i < RHICompletionSet::kQueueCount; ++i)
        {
            const RHICommandContextType queue = static_cast<RHICommandContextType>(i);
            result.Extend(queue, QueryLastCompletedSerial(queue));
        }
        return result;
    }

    virtual IRHICommandContext* GetCommandContext(RHICommandContextType contextType) = 0;

    virtual RHIAPIType GetAPIType()                                                  = 0;

    virtual std::string_view GetName()                                               = 0;

    virtual DataFormat GetSupportedDepthFormat()                                     = 0;

    // Called on the window-owning thread. Backends prepare the native surface here
    // and dispatch swapchain/backbuffer initialization to RHI.
    virtual RHIViewport* CreateViewport(zen::RHIWindowHandle pWindow, uint32_t width, uint32_t height, bool enableVSync) = 0;

    // Called on the window owner after outstanding frames drain, as for CreateViewport/Resize.
    // The owner may destroy the native window next, so backends release everything bound to it
    // before returning. Other GPU resource destruction runs through RHI cleanup.
    virtual void DestroyViewport(RHIViewport* pViewport)                   = 0;

    virtual RHIShader* CreateShader(const RHIShaderCreateInfo& createInfo) = 0;

    void DestroyShader(RHIShader* pShader)
    {
        ReleaseResource(pShader);
    }

    virtual RHIPipeline* CreatePipeline(const RHIComputePipelineCreateInfo& createInfo) = 0;

    virtual RHIPipeline* CreatePipeline(const RHIGfxPipelineCreateInfo& createInfo)     = 0;

    virtual RHIPipeline* CreatePipeline(const RHIRayTracingPipelineCreateInfo& createInfo)
    {
        return nullptr;
    }

    // Snapshot a healthy device. The caller bounds the complete blob and owns its persistence.
    virtual RHIError SnapshotPipelineCache(size_t byteLimit, std::vector<uint8_t>& bytes)
    {
        return MakeRHIError(RHIErrorCode::eUnsupported, "SnapshotPipelineCache", __FILE__, __LINE__);
    }

    void DestroyPipeline(RHIPipeline* pPipeline)
    {
        ReleaseResource(pPipeline);
    }

    virtual RHISampler* CreateSampler(const RHISamplerCreateInfo& createInfo) = 0;

    void DestroySampler(RHISampler* pSampler)
    {
        ReleaseResource(pSampler);
    }

    virtual RHIBindlessHandle RegisterBindlessResource(RHIResource* pResource, uint32_t slotIndex = kInvalidBindlessSlotIndex)
    {
        return {};
    }

    // Stop publishing this index to new work before unregistering. Already
    // recorded/submitted users retain the resource and prevent slot reuse.
    virtual bool UnregisterBindlessResource(RHIBindlessHandle handle)
    {
        return false;
    }

    virtual bool IsBindlessResourceRegistered(RHIBindlessHandle handle)
    {
        return false;
    }

    // Poll completion and release eligible retired registrations without waiting.
    virtual void CollectRetiredBindlessResources() {}

    // Explicitly replace a complete heap at a drained resource boundary. Live CPU recordings
    // or GPU submissions reject the reset; success invalidates all previous registrations.
    virtual bool ResetBindlessResources()
    {
        return true;
    }

    virtual RHITexture* CreateTexture(const RHITextureCreateInfo& createInfo)                                       = 0;

    virtual RHITextureView* CreateTextureView(RHITexture* pBaseTexture, const RHITextureViewCreateInfo& createInfo) = 0;

    void DestroyTexture(RHITexture* pTexture)
    {
        ReleaseResource(pTexture);
    }

    virtual RHIAccelerationStructureBuildSizes GetAccelerationStructureBuildSizes(const RHIAccelerationStructureBuildDesc& desc)
    {
        return {};
    }

    virtual RHIAccelerationStructure* CreateAccelerationStructure(const RHIAccelerationStructureCreateInfo& info)
    {
        return nullptr;
    }

    virtual RHIBuffer* CreateBuffer(const RHIBufferCreateInfo& createInfo) = 0;

    // Native buffer allocation extent, including resource padding, without allocating buffer memory.
    // Zero means unsupported or failure. Excludes allocator pool commitment and driver-private memory.
    virtual uint64_t GetBufferAllocationSize(const RHIBufferCreateInfo&)
    {
        return 0;
    }

    void DestroyBuffer(RHIBuffer* pBuffer)
    {
        ReleaseResource(pBuffer);
    }

    // Translates and submits the lists in order, then any other work pending on the queues. Backends keep
    // marked group boundaries and publish each list's accepted serial before returning; the lists stay alive
    // until then. A translation failure rejects the whole batch. Rejected work is never retried implicitly.
    // GetLastSubmissionError() names the cause of a failure.
    virtual RHISubmissionResult SubmitCommandLists(std::span<RHICommandList*> lists) = 0;

    // Submits the work already pending on the queues.
    RHISubmissionResult FlushAllGPUCommands()
    {
        return SubmitCommandLists({});
    }

    // Terminal backend failure, including failures discovered while polling GPU progress.
    // Raw backends are queried on their owning thread; executors publish a thread-safe snapshot.
    virtual bool AreSubmissionsBlocked() const
    {
        return false;
    }

    virtual RHIError GetTerminalError() const
    {
        return AreSubmissionsBlocked() ? MakeRHIError(RHIErrorCode::eBackendFailure, "RHI backend stopped", __FILE__, __LINE__)
                                       : RHIError{};
    }

    virtual bool HasDeviceLoss() const
    {
        return GetTerminalError().code == RHIErrorCode::eDeviceLost;
    }

    // Read on the backend owner immediately after a rejected native submission.
    virtual RHIError GetLastSubmissionError() const
    {
        return GetTerminalError();
    }

    virtual RHIQueueCapabilities GetQueueCapabilities() const
    {
        return {};
    }

    // Attach dependencies before recording/finalizing the consumer context. Producers must
    // already be submitted; kLatestSubmitted resolves here, never to future submissions.
    // Backends without GPU waits retain a completion-wait fallback. Failure blocks the batch.
    virtual bool PrepareSubmissionDependencies(IRHICommandContext*                      context,
                                               std::span<const RHISubmissionDependency> dependencies)
    {
        bool result = context != nullptr;
        for (const RHISubmissionDependency& dependency : dependencies)
        {
            if (result)
            {
                const bool     validQueue = dependency.queue < RHICommandContextType::eMax;
                const uint64_t submitted  = validQueue ? GetLastSubmittedSerial(dependency.queue) : 0;
                const uint64_t serial =
                    dependency.serial == RHISubmissionDependency::kLatestSubmitted ? submitted : dependency.serial;
                result                               = validQueue && serial <= submitted;
                const RHICommandContextType consumer = context->GetContextType();
                const bool                  shared   = GetQueueCapabilities().AreQueuesShared(dependency.queue, consumer);
                if (result && !shared && QueryLastCompletedSerial(dependency.queue) < serial)
                {
                    result = static_cast<bool>(WaitForCompletion(dependency.queue, serial));
                }
            }
        }
        return result;
    }

    virtual uint64_t GetLastSubmittedSerial(RHICommandContextType contextType) const = 0;

    // May poll native progress. For cached-only reads, use the executor's GetCached* APIs.
    virtual uint64_t QueryLastCompletedSerial(RHICommandContextType contextType) = 0;

    // Wait only for an already-submitted queue serial. Zero is already complete; zero timeout
    // polls. False means timeout/failure and must never be treated as permission to reuse work.
    virtual RHIWaitResult WaitForCompletion(RHICommandContextType contextType,
                                            uint64_t              submissionSerial,
                                            uint64_t              timeoutNS = UINT64_MAX) = 0;

    virtual void WaitDeviceIdle()                                                         = 0;

    virtual const RHIGPUInfo& QueryGPUInfo() const                                        = 0;

    // The description the backend was created with. Immutable, so any thread may read it.
    virtual const RHIDeviceDesc& GetDeviceDesc() const = 0;

    // The worker, frame pacing and destruction listener of the device. An executor returns its backend's.
    virtual RHIDeviceRuntime& GetRuntime() = 0;

    // A borrowed native handle, or zero when the backend has none. Valid between initialization and teardown.
    virtual uint64_t GetNativeObject(RHINativeObject object) const
    {
        return 0;
    }

    // Safe to read while the initialized backend is alive, including from the render
    // thread. Reads published counters only; never waits for RHI/GPU work or polls a driver.
    virtual RHIGPUMemoryStats GetGPUMemoryStats() const
    {
        return {};
    }

    virtual RHITextureCopyCapabilities GetTextureCopyCapabilities(DataFormat format) const      = 0;

    virtual RHIQueueCopyCapabilities GetQueueCopyCapabilities(RHICommandContextType type) const = 0;

protected:
    // A wait reports failure whenever the backend has stopped, even if the serial completed earlier.
    RHIWaitResult MakeWaitResult(bool completed) const
    {
        const RHIError error = GetTerminalError();

        return {error.IsFailure() ? RHIWaitOutcome::eFailed
                : completed       ? RHIWaitOutcome::eCompleted
                                  : RHIWaitOutcome::eIncomplete,
                error};
    }

private:
    // Destroy* releases one reference. Final destruction runs on the RHI thread after earlier queued work.
    static void ReleaseResource(RHIResource* pResource)
    {
        if (pResource != nullptr)
        {
            pResource->ReleaseReference();
        }
    }
};
} // namespace zen
