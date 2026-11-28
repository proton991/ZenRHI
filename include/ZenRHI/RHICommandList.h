#pragma once
#include "ZenRHI/RHIDeviceRuntime.h"
#include "ZenRHI/RHIDiagnostics.h"
#include <span>
#include "RHIError.h"
#include "RHIResource.h"
#include "RHIAccelerationStructure.h"
#include "RHIShaderParameters.h"
#include "RHIGPUTiming.h"
#include "ZenFoundation/Memory/PoolAllocator.h"
#include "ZenFoundation/Memory/LinearAllocator.h"
#include "ZenFoundation/Memory/Memory.h"
#include "ZenFoundation/Templates/HeapVector.h"
#include "ZenFoundation/Templates/SmallVector.h"
#include "ZenFoundation/Templates/VectorView.h"
#include <cstring>
#include <type_traits>
#include <utility>
#include <memory>
#include <algorithm>
#include "ZenFoundation/Utils/RefCountPtr.h"
#include "ZenFoundation/Templates/FlatHashMap.h"

namespace zen
{
class RHIPipeline;
class RHICommandList;

struct RHICommandListDeleter
{
    void operator()(RHICommandList* commands) const;
};

using RHICommandListPtr = std::unique_ptr<RHICommandList, RHICommandListDeleter>;

class RHIResourceReferences
{
public:
    RHIResourceReferences() = default;

    ~RHIResourceReferences();

    RHIResourceReferences(const RHIResourceReferences&)            = delete;

    RHIResourceReferences& operator=(const RHIResourceReferences&) = delete;

    RHIResourceReferences(RHIResourceReferences&& other) noexcept;

    RHIResourceReferences& operator=(RHIResourceReferences&& other) noexcept;

    void Retain(RHIResource* resource);

    void Reset();

    void Swap(RHIResourceReferences& other);

    size_t GetCount() const;

    void Rollback(size_t count);

private:
    HeapVector<RHIResource*>          m_resources;
    FlatHashMap<RHIResource*, size_t> m_unique;
};

// Recorded commands live in the list's arena and are destroyed through this virtual destructor.
struct RHICommand
{
    RHICommand* pNextCmd{nullptr};

    virtual ~RHICommand()                         = default;

    virtual void Execute(RHICommandList& cmdList) = 0;
};

enum class RHICommandContextType : uint32_t
{
    eGraphics     = 0,
    eAsyncCompute = 1,
    eTransfer     = 2,
    eMax          = 3
};

inline const char* RHIQueueName(RHICommandContextType queue)
{
    const char* name = "graphics";
    if (queue == RHICommandContextType::eAsyncCompute)
    {
        name = "compute";
    }
    else if (queue == RHICommandContextType::eTransfer)
    {
        name = "transfer";
    }
    return name;
}

// Serial values belong to their logical queue's timeline, even when native queues alias.
struct RHICompletionSet
{
    static constexpr size_t           kQueueCount = static_cast<size_t>(RHICommandContextType::eMax);
    std::array<uint64_t, kQueueCount> serials{};

    uint64_t Get(RHICommandContextType queue) const
    {
        return serials[static_cast<size_t>(queue)];
    }

    void Extend(RHICommandContextType queue, uint64_t serial)
    {
        uint64_t& required = serials[static_cast<size_t>(queue)];
        required           = std::max(required, serial);
    }

    void Extend(const RHICompletionSet& other)
    {
        for (size_t i = 0; i < kQueueCount; ++i)
        {
            serials[i] = std::max(serials[i], other.serials[i]);
        }
    }

    bool IsCompleteAt(const RHICompletionSet& completed) const
    {
        bool ready = true;
        for (size_t i = 0; i < kQueueCount; ++i)
        {
            ready &= completed.serials[i] >= serials[i];
        }
        return ready;
    }

    void Reset()
    {
        serials.fill(0);
    }
};

struct RHIQueueCapabilities
{
    bool computeSupported{false};
    bool asyncSubmissionDependencies{false};
    // Equal IDs identify one native queue. They do not identify a shared serial timeline.
    std::array<uint32_t, RHICompletionSet::kQueueCount> queueIds{};

    bool AreQueuesShared(RHICommandContextType first, RHICommandContextType second) const
    {
        const size_t firstIndex  = static_cast<size_t>(first);
        const size_t secondIndex = static_cast<size_t>(second);
        return firstIndex < queueIds.size() && secondIndex < queueIds.size() && queueIds[firstIndex] == queueIds[secondIndex];
    }

    // Representative index for access history; completion serials keep their logical index.
    size_t GetNativeQueueIndex(RHICommandContextType queue) const
    {
        const size_t logical = static_cast<size_t>(queue);
        size_t       result  = logical;
        for (size_t i = 0; logical < queueIds.size() && i < logical; ++i)
        {
            if (queueIds[i] == queueIds[logical])
            {
                result = i;
                break;
            }
        }
        return result;
    }

    bool SupportsAsyncCompute() const
    {
        return computeSupported && asyncSubmissionDependencies
            && !AreQueuesShared(RHICommandContextType::eGraphics, RHICommandContextType::eAsyncCompute);
    }
};

struct RHISubmissionDependency
{
    static constexpr RHIPipelineStageFlagBits kWaitStage = RHIPipelineStageFlagBits::eAllCommands;
    // Resolve on the submission thread, after previously queued CPU batches have executed.
    static constexpr uint64_t kLatestSubmitted = UINT64_MAX;
    RHICommandContextType     queue{RHICommandContextType::eGraphics};
    uint64_t                  serial{0};
};

class IRHICommandContext : public RefCounted, public RHIDeviceChild
{
public:
    // Optional profiler annotations; no resource or synchronization semantics.
    virtual void RHIBeginDebugLabel(std::string_view name) {}

    virtual void RHIEndDebugLabel() {}

    virtual void RHIBeginGPUTiming(const RHIGPUTimingPtr& result)
    {
        if (result != nullptr)
        {
            result->Publish(RHIGPUTimingStatus::eUnsupported);
        }
    }

    virtual void RHIEndGPUTiming(const RHIGPUTimingPtr& result) {}

    // Resource arguments are borrowed through GPU completion (including the base
    // texture of a view). Recorded rendering layouts must survive command execution.
    virtual RHICommandContextType GetContextType() = 0;

    virtual ~IRHICommandContext() {}

    virtual RHIError RHIGetRecordingError() const
    {
        return {};
    }

    // Non-virtual equivalent of RHIGetRecordingError().IsFailure() for per-command execution.
    bool HasRecordingError() const
    {
        return m_pRecordingError != nullptr && m_pRecordingError->IsFailure();
    }

    virtual void RHIDiscardRecording() {}

    virtual void RHIBeginRendering(const RHIRenderingLayout* pRenderingLayout)                                    = 0;

    virtual void RHIEndRendering()                                                                                = 0;

    virtual void RHISetScissor(uint32_t minX, uint32_t minY, uint32_t maxX, uint32_t maxY)                        = 0;

    virtual void RHISetViewport(uint32_t minX, uint32_t minY, uint32_t maxX, uint32_t maxY)                       = 0;

    virtual void RHISetDepthBias(float depthBiasConstantFactor, float depthBiasClamp, float depthBiasSlopeFactor) = 0;

    virtual void RHISetLineWidth(float lineWidth)                                                                 = 0;

    virtual void RHISetBlendConstants(const Color& blendConstants)                                                = 0;

    virtual void RHIBindPipeline(RHIPipeline* pPipeline)                                                          = 0;

    virtual void RHISetShaderParameters(RHIShaderParameterView parameters)                                        = 0;

    // Capture holds a recording epoch until the matching release. Zero is invalid.
    virtual uint64_t RHIGetCurrentBindlessEpoch() const
    {
        return 0;
    }

    virtual uint64_t RHICaptureBindlessEpoch()
    {
        return 0;
    }

    virtual void RHIReleaseBindlessEpoch(uint64_t epoch) {}

    // Scoped by command execution; the recorded command keeps this epoch alive.
    virtual void RHISetRecordedBindlessEpoch(uint64_t epoch) {}

    virtual void RHIBindVertexBuffers(std::span<RHIBuffer*> pBuffers, std::span<uint64_t> offsets)                   = 0;

    virtual void RHIBindVertexBuffer(RHIBuffer* pBuffer, uint64_t offset)                                            = 0;

    virtual void RHIDraw(uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex, uint32_t firstInstance) = 0;

    virtual void RHIDrawIndexed(RHIBuffer* pIndexBuffer,
                                DataFormat indexFormat,
                                uint64_t   indexBufferOffset,
                                uint32_t   indexCount,
                                uint32_t   instanceCount,
                                uint32_t   firstIndex,
                                int32_t    vertexOffset,
                                uint32_t   firstInstance)                                                            = 0;

    virtual void RHIDrawIndexedIndirect(RHIBuffer* pIndirectBuffer,
                                        RHIBuffer* pIndexBuffer,
                                        DataFormat indexFormat,
                                        uint64_t   indexBufferOffset,
                                        uint64_t   offset,
                                        uint32_t   drawCount,
                                        uint32_t   stride)                                                           = 0;

    virtual void RHIDispatch(uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ)                       = 0;

    virtual void RHIDispatchIndirect(RHIBuffer* pIndirectBuffer, uint64_t offset)                                    = 0;

    virtual void RHITraceRays(uint32_t width, uint32_t height, uint32_t depth)                                       = 0;

    // The indirect buffer contains three uint32_t launch dimensions, and requires
    // indirect + device-address usage. Its producer must obey the device launch limits.
    virtual void RHITraceRaysIndirect(RHIBuffer* pIndirectBuffer, uint64_t offset)                               = 0;

    virtual void RHISetPushConstants(RHIPipeline* pPipeline, std::span<const uint8_t> data, uint32_t offset = 0) = 0;

    virtual void RHIAddTransitions(RHIFlags<RHIPipelineStageFlagBits> srcStages,
                                   RHIFlags<RHIPipelineStageFlagBits> dstStages,
                                   std::span<RHIMemoryTransition>     memoryTransitions,
                                   std::span<RHIBufferTransition>     bufferTransitions,
                                   std::span<RHITextureTransition>    textureTransitions)                        = 0;

    virtual void RHIBuildAccelerationStructure(const RHIAccelerationStructureBuildInfo& info)
    {
        ZEN_RHI_VERIFY_MSG(false, "Acceleration structure builds are unsupported by this context");
    }

    virtual void RHIClearBuffer(RHIBuffer* pBuffer, uint64_t offset, uint64_t size)                                        = 0;

    virtual void RHICopyBuffer(RHIBuffer* pSrcBuffer, RHIBuffer* pDstBuffer, const RHIBufferCopyRegion& region)            = 0;

    virtual void RHIClearTexture(RHITexture* pTex, const Color& color, const RHITextureSubresourceRange& range)            = 0;

    virtual void RHICopyTexture(RHITexture* pSrcTexture, RHITexture* pDstTexture, std::span<RHITextureCopyRegion> regions) = 0;

    virtual void RHIBlitTexture(RHITexture*                     pSrcTexture,
                                RHITexture*                     pDstTexture,
                                std::span<RHITextureBlitRegion> regions,
                                RHISamplerFilter                filter)                                                    = 0;

    virtual void RHICopyTextureToBuffer(RHITexture*                           pSrcTex,
                                        RHIBuffer*                            pDstBuffer,
                                        std::span<RHIBufferTextureCopyRegion> regions)                                     = 0;

    virtual void RHICopyBufferToTexture(RHIBuffer*                            pSrcBuffer,
                                        RHITexture*                           pDstTexture,
                                        std::span<RHIBufferTextureCopyRegion> regions)                                     = 0;

    virtual void RHIResolveTexture(RHITexture* pSrcTexture,
                                   RHITexture* pDstTexture,
                                   uint32_t    srcLayer,
                                   uint32_t    srcMipmap,
                                   uint32_t    dstLayer,
                                   uint32_t    dstMipmap)                                                                  = 0;


protected:
    // A context that latches recording errors binds its error record for HasRecordingError.
    // The record must live as long as the context.
    void BindRecordingError(const RHIError* error)
    {
        m_pRecordingError = error;
    }

private:
    void OnFinalRelease() override final;

    const RHIError* m_pRecordingError{nullptr};
};

// Indexed draw arguments. The index buffer is retained by the recording command list.
struct RHIDrawIndexedArgs
{
    RHIBuffer* pIndexBuffer{nullptr};
    DataFormat indexFormat{};
    uint64_t   indexBufferOffset{0};
    uint32_t   indexCount{0};
    uint32_t   instanceCount{0};
    uint32_t   firstIndex{0};
    int32_t    vertexOffset{0};
    uint32_t   firstInstance{0};
};

// Indexed indirect draw arguments. Both buffers are retained by the recording command list.
struct RHIDrawIndexedIndirectArgs
{
    RHIBuffer* pIndirectBuffer{nullptr};
    RHIBuffer* pIndexBuffer{nullptr};
    DataFormat indexFormat{};
    uint64_t   indexBufferOffset{0};
    uint64_t   offset{0};
    uint32_t   drawCount{0};
    uint32_t   stride{0};
};

class RHICommandList final
{
public:
    static RHICommandList* Create(IRHICommandContext* pContext);

    RHICommandList() : m_cmdAllocator(64 * 1024)
    {
        m_ppCmdPtr = &m_pCmdHead;
    }

    ~RHICommandList()
    {
        Reset();
        m_ppCmdPtr = nullptr;
    }

    RHICommandList(const RHICommandList&)            = delete;

    RHICommandList& operator=(const RHICommandList&) = delete;

    template <typename T, typename... Args> T* AllocateCmdTyped(Args&&... args)
    {
        static_assert(std::is_base_of_v<RHICommand, T>, "T must derive from RHICommand");
        // Construct command metadata before linking it into the recorded command list.
        void* memory = m_cmdAllocator.Alloc(sizeof(T), alignof(T));
        T*    pCmd   = new (memory) T(std::forward<Args>(args)...);
        *m_ppCmdPtr  = pCmd;
        m_ppCmdPtr   = &pCmd->pNextCmd;
        ++m_numCommands;

        return pCmd;
    }

    template <typename T> T* AllocateCmdData(size_t count)
    {
        T* result{};

        if (count != 0)
        {
            result = static_cast<T*>(m_cmdAllocator.Alloc(sizeof(T) * count, alignof(T)));
        }

        return result;
    }

    template <typename T> std::span<std::remove_const_t<T>> CopyCmdData(std::span<T> source)
    {
        static_assert(std::is_trivially_copyable_v<T>);

        std::remove_const_t<T>* storage = AllocateCmdData<std::remove_const_t<T>>(source.size());

        if (!source.empty())
        {
            std::memcpy(storage, source.data(), source.size() * sizeof(T));
        }

        return MakeVecView(storage, source.size());
    }

    IRHICommandContext* GetContext() const
    {
        return m_contextOwner.Get();
    }

    uint64_t CaptureBindlessEpoch();

    void ReleaseBindlessEpochs(size_t count);

    void Execute();

    void Reset();

    void RetainResource(RHIResource* resource)
    {
        m_resources.Retain(resource);
    }

    void AddSubmissionDependency(RHISubmissionDependency dependency)
    {
        if (dependency.serial != 0)
        {
            m_submissionDependencies.push_back(dependency);
        }
    }

    std::span<const RHISubmissionDependency> GetSubmissionDependencies() const
    {
        return m_submissionDependencies;
    }

    // Executor/backend metadata, accessed only on the RHI thread. A group boundary
    // keeps this list's completion signal separate when native calls are packed.
    void SetSubmissionBoundary(bool boundary)
    {
        m_submissionBoundary = boundary;
    }

    bool HasSubmissionBoundary() const
    {
        return m_submissionBoundary;
    }

    // Flush publishes this list's actual accepted work, independently of its context.
    // Zero means no native workload was accepted for this list.
    void SetSubmittedSerial(uint64_t serial)
    {
        m_submittedSerial = serial;
    }

    uint64_t GetSubmittedSerial() const
    {
        return m_submittedSerial;
    }

    struct CommandCheckpoint
    {
        RHICommand** tail;
        uint32_t     count;
        size_t       resourceCount;
        size_t       dependencyCount;
        size_t       captureCount;
    };

    CommandCheckpoint GetCommandCheckpoint() const
    {
        return {m_ppCmdPtr, m_numCommands, m_resources.GetCount(), m_submissionDependencies.size(), m_bindlessEpochs.size()};
    }

    uint32_t GetCommandCount() const
    {
        return m_numCommands;
    }

    // Discard only commands appended after this checkpoint. Arena bytes are reclaimed by Reset.
    void RollbackCommands(CommandCheckpoint checkpoint);

    // Moves only CPU recording storage. The reusable producer and detached batch
    // share a context whose native state is accessed exclusively by the executor.
    // Optional empty storage supplies the producer's replacement arena without allocating.
    RHICommandListPtr DetachCommands(RHICommandListPtr reusable = {});

    // Called on RHI after retirement. Keep CPU storage, but release the producer's context.
    void ResetForReuse();

    void ClearBuffer(RHIBuffer* pBuffer, uint64_t offset, uint64_t size);

    void BuildAccelerationStructure(const RHIAccelerationStructureBuildInfo& info);

    void CopyBuffer(RHIBuffer* pSrcBuffer, RHIBuffer* pDstBuffer, const RHIBufferCopyRegion& region);

    void ClearTexture(RHITexture* pTexture, const Color& color, const RHITextureSubresourceRange& range);

    void CopyTexture(RHITexture*                           pSrcTextureHandle,
                     RHITexture*                           pDstTextureHandle,
                     std::span<const RHITextureCopyRegion> regions);

    void BlitTexture(RHITexture*                     pSrcTextureHandle,
                     RHITexture*                     pDstTextureHandle,
                     std::span<RHITextureBlitRegion> regions,
                     RHISamplerFilter                filter);

    void CopyTextureToBuffer(RHITexture* pSrcTex, RHIBuffer* pDstBuffer, std::span<RHIBufferTextureCopyRegion> regions);

    void CopyBufferToTexture(RHIBuffer*                                  pSrcBuffer,
                             RHITexture*                                 pDstTexture,
                             std::span<const RHIBufferTextureCopyRegion> regions);

    void ResolveTexture(RHITexture* pSrcTexture,
                        RHITexture* pDstTexture,
                        uint32_t    srcLayer,
                        uint32_t    srcMipmap,
                        uint32_t    dstLayer,
                        uint32_t    dstMipmap);

    void SetViewport(uint32_t minX, uint32_t minY, uint32_t maxX, uint32_t maxY);

    void SetScissor(uint32_t minX, uint32_t minY, uint32_t maxX, uint32_t maxY);

    void SetDepthBias(float depthBiasConstantFactor, float depthBiasClamp, float depthBiasSlopeFactor);

    void SetLineWidth(float width);

    void BeginDebugLabel(std::string_view name);

    void EndDebugLabel();

    void BeginGPUTiming(const RHIGPUTimingPtr& result);

    void EndGPUTiming(const RHIGPUTimingPtr& result);

    void SetBlendConstants(const Color& color);

    void BindPipeline(RHIPipeline* pPipeline);

    void SetShaderParameters(const RHIBatchedShaderParameters& parameters);

    void BeginRendering(const RHIRenderingLayout* pRenderingLayout);

    void EndRendering();

    void BindVertexBuffers(std::span<RHIBuffer*> vertexBuffers, std::span<uint64_t> offsets);

    // All vertex attributes packed in 1 buffer
    void BindVertexBuffer(RHIBuffer* pBuffer, uint64_t offset);

    void Draw(uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex, uint32_t firstInstance);

    void DrawIndexed(const RHIDrawIndexedArgs& args);

    void DrawIndexedIndirect(const RHIDrawIndexedIndirectArgs& args);

    void Dispatch(uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ);

    void DispatchIndirect(RHIBuffer* pIndirectBuffer, uint64_t offset);

    void TraceRays(uint32_t width, uint32_t height, uint32_t depth = 1);

    void TraceRaysIndirect(RHIBuffer* pIndirectBuffer, uint64_t offset);

    void SetPushConstants(RHIPipeline* pPipeline, const uint8_t* pData, uint32_t sizeBytes, uint32_t offset);

    void AddTransitions(RHIFlags<RHIPipelineStageFlagBits> srcStages,
                        RHIFlags<RHIPipelineStageFlagBits> dstStages,
                        std::span<RHIMemoryTransition>     memoryTransitions,
                        std::span<RHIBufferTransition>     bufferTransitions,
                        std::span<RHITextureTransition>    textureTransitions);

private:
    RHICommand*  m_pCmdHead{nullptr};
    RHICommand** m_ppCmdPtr{nullptr};

    uint32_t                            m_numCommands{0};
    PoolAllocator<LinearAllocator>      m_cmdAllocator;
    RefCountPtr<IRHICommandContext>     m_contextOwner;
    RHIResourceReferences               m_resources;
    HeapVector<RHISubmissionDependency> m_submissionDependencies;
    SmallVector<uint64_t, 4>            m_bindlessEpochs;
    uint64_t                            m_submittedSerial{0};
    bool                                m_submissionBoundary{false};
};

} // namespace zen
