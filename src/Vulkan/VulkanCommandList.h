#pragma once
#include "ZenFoundation/Templates/BitMask.h"
#include <span>
#include "VulkanQueue.h"
#include "VulkanRHI.h"
#include "Vulkan/VulkanDescriptorAllocator.h"
#include "VulkanDescriptorState.h"
#include "VulkanMemory.h"
#include "Vulkan/VulkanCommandRecorders.h"
#include "ZenFoundation/Templates/HeapVector.h"
#include "ZenFoundation/Templates/VectorView.h"
#include "ZenFoundation/Utils/Mutex.h"

namespace zen
{
class VulkanDescriptorSetState;
class VulkanBuffer;
class VulkanPipeline;
class VulkanCommandBufferPool;
class VulkanSemaphore;
class VulkanQueue;
struct RHIRenderingLayout;
class VulkanCommandListContext;

class VulkanCommandBuffer
{
public:
    enum class State : uint8_t
    {
        eReadyForBegin,
        eIsInsideBegin,
        eIsInsideRenderPass,
        eHasEnded,
        eSubmitted,
        eNotAllocated,
        eNeedReset,
    };

    bool Begin();

    bool End();

    void BeginBreadcrumb(NameID name)
    {
        m_breadcrumbs.Begin(*this, name);
    }

    void EndBreadcrumb()
    {
        m_breadcrumbs.End(*this);
    }

    void ReportBreadcrumbs(VulkanFaultReport& report) const
    {
        m_breadcrumbs.Report(*this, report);
    }

    VulkanDevice* GetDevice() const
    {
        return m_pDevice;
    }

    const RHIError& GetError() const
    {
        return m_error;
    }

    void BeginRendering(const VkRenderingInfo* pRenderingInfo);

    void EndRendering();

    // Call after external native commands change pipeline, descriptor, vertex or dynamic state.
    // Begin also invalidates all cached state, including when a native handle is recycled.
    void InvalidateCachedState();

    void BindIndexBuffer(VkBuffer buffer, uint64_t offset, VkIndexType type);

    void BindPipelineAndDescriptorSets(VulkanPipeline*                    pipeline,
                                       const HeapVector<VkDescriptorSet>& sets,
                                       uint32_t                           firstSet,
                                       const HeapVector<uint32_t>&        offsets);

    void SetViewport(const VkViewport& viewport);

    void SetScissor(const VkRect2D& scissor);

    void SetDepthBias(float constantFactor, float clamp, float slopeFactor);

    void SetLineWidth(float width);

    void SetBlendConstants(const float* values);

    void BindVertexBuffers(const HeapVector<VkBuffer>& buffers, const HeapVector<uint64_t>& offsets);

    void SetSubmitted();

    // The owner must know this recording was not submitted; repeated discard is harmless.
    void Discard();

    void SetCompleted();

    void BeginGPUTiming(const RHIGPUTimingPtr& result)
    {
        m_timing.Begin(*this, result);
    }

    void EndGPUTiming(const RHIGPUTimingPtr& result)
    {
        m_timing.End(*this, result);
    }

    bool IsInsideRenderPass() const
    {
        return m_state == State::eIsInsideRenderPass;
    }

    bool IsOutsideRenderPass() const
    {
        return m_state == State::eIsInsideBegin;
    }

    bool HasBegun() const
    {
        return m_state == State::eIsInsideBegin || m_state == State::eIsInsideRenderPass;
    }

    bool HasEnded() const
    {
        return m_state == State::eHasEnded;
    }

    bool IsSubmitted() const
    {
        return m_state == State::eSubmitted;
    }

    bool IsAllocated() const
    {
        return m_state != State::eNotAllocated;
    }

    VkCommandBuffer GetVkHandle() const
    {
        return m_vkHandle;
    }

    VulkanCommandBufferPool* GetCommandBufferPool() const
    {
        return m_pCmdBufferPool;
    }

protected:
    friend class VulkanCommandBufferPool;
    friend class VulkanCommandContextBase;

    VulkanCommandBuffer(VulkanCommandBufferPool* pPool);

    ~VulkanCommandBuffer();

private:
    void AllocMemory();

    void FreeMemory();

    VkCommandBuffer m_vkHandle{VK_NULL_HANDLE};

    VulkanCommandBufferPool* m_pCmdBufferPool{nullptr};

    VulkanDevice* m_pDevice{nullptr};

    State m_state{State::eNotAllocated};

    RHIError                 m_error{};
    VulkanBreadcrumbRecorder m_breadcrumbs;
    VulkanGPUTimingRecorder  m_timing;

    double m_submitTime{0.0f};



    struct BoundPipelineState
    {
        VkPipeline                  pipeline{VK_NULL_HANDLE};
        VkPipelineLayout            descriptorLayout{VK_NULL_HANDLE};
        uint32_t                    firstSet{0};
        HeapVector<VkDescriptorSet> descriptorSets;
        HeapVector<uint32_t>        dynamicOffsets;
    };
    // Graphics and compute bind points have independent state.
    BoundPipelineState           m_boundStates[3];
    EnumBitMask<RHIDynamicState> m_validDynamicStates;
    VkViewport                   m_viewport{};
    VkRect2D                     m_scissor{};
    float                        m_depthBias[3]{};
    float                        m_lineWidth{1.0f};
    std::array<float, 4>         m_blendConstants{};
    HeapVector<VkBuffer>         m_boundVertexBuffers;
    HeapVector<uint64_t>         m_boundVertexOffsets;

    VkBuffer    m_boundIndexBuffer{VK_NULL_HANDLE};
    uint64_t    m_boundIndexOffset{0};
    VkIndexType m_boundIndexType{VK_INDEX_TYPE_UINT16};
};

class VulkanCommandBufferPool
{
    friend class VulkanCommandContextBase;

public:
    explicit VulkanCommandBufferPool(VulkanQueue* pQueue);

    ~VulkanCommandBufferPool();

    void FreeUnusedCommandBuffers();

    Mutex* GetMutex()
    {
        return &m_mutex;
    }

    VkCommandPool GetVkHandle() const
    {
        return m_vkHandle;
    }

    VulkanQueue* GetQueue() const
    {
        return m_pQueue;
    }

private:
    VulkanCommandBuffer* CreateCmdBuffer();

    Mutex m_mutex;

    VulkanQueue* m_pQueue{nullptr};

    VkCommandPool m_vkHandle{VK_NULL_HANDLE};

    HeapVector<VulkanCommandBuffer*> m_cmdBuffersInUse;
    HeapVector<VulkanCommandBuffer*> m_cmdBuffersFree;
};

// Native submission state for one VulkanQueue submission.
class VulkanWorkload
{
    friend class VulkanCommandContextBase;
    friend class VulkanQueue;
    friend class VulkanRHI;

public:
    explicit VulkanWorkload(VulkanQueue* pQueue) : m_pQueue(pQueue) {}

    ~VulkanWorkload();

    struct WaitSemaphoreInfo
    {
        VkPipelineStageFlags waitFlags{0};
        VulkanSemaphore*     pSemaphore{nullptr};
        uint64_t             value{0};
    };

    struct SignalSemaphoreInfo
    {
        VulkanSemaphore* pSemaphore{nullptr};
        uint64_t         value{0};
    };

private:
    VulkanCommandBuffer* GetLastCommandBuffer() const;

    bool HasCommandBuffers() const
    {
        return !m_commandBuffers.empty();
    }

    void AddCommandBuffer(VulkanCommandBuffer* pCmdBuffer);

    void Merge(VulkanWorkload* pOtherWorkload);

    VulkanQueue*                     m_pQueue{nullptr};
    HeapVector<VulkanCommandBuffer*> m_commandBuffers;
    uint64_t                         m_submissionSerial{0};
    bool                             m_submissionBoundary{false};
    VulkanWorkload*                  m_pMergedInto{nullptr};
    HeapVector<VulkanWorkload*>      m_mergedWorkloads;

    // What the workload keeps alive until its queue serial completes. Merging moves the recordings to the root;
    // each workload keeps its own references and bindless transaction.
    struct Ownership
    {
        HeapVector<uint64_t>  lifetimeIds;
        RHIResourceReferences resources;
        uint64_t              bindlessTransaction{0};
        bool                  ownsBindlessTransaction{false};

        // After a successful submission: the recordings follow the serial and an owned transaction commits.
        void Accept(VulkanRHI& rhi, const VulkanQueue* pQueue, uint64_t serial);

        // Without a submission: the recordings, references and an owned transaction are dropped.
        void Release(VulkanRHI& rhi);
    } m_ownership;

    // DO NOT own the semaphores, only hold reference
    HeapVector<WaitSemaphoreInfo>   m_waitSemaphoreInfos;
    HeapVector<SignalSemaphoreInfo> m_signalSemaphoreInfos;
};

// manages VulkanWorkload
// holds VulkanCommandBufferPool
class VulkanCommandContextBase
{
public:
    explicit VulkanCommandContextBase(VulkanQueue* pQueue) :
        m_pQueue(pQueue), m_pCmdBufferPool(pQueue->AcquireCommandBufferPool())
    {}

    virtual ~VulkanCommandContextBase();

    VulkanCommandBuffer* GetCommandBuffer()
    {
        VulkanWorkload* pWorkload = GetWorkload(WorkloadPhase::eExecute);

        if (!pWorkload->HasCommandBuffers())
        {
            SetupNewCommandBuffer();
        }

        return pWorkload->GetLastCommandBuffer();
    }

    bool IsValid() const
    {
        return m_pCmdBufferPool->GetVkHandle() != VK_NULL_HANDLE;
    }

    const RHIError& GetRecordingError() const
    {
        return m_recordingError;
    }

    void LatchError(RHIError error);

    bool EnsureRecording();

    virtual void DiscardRecording();

    uint64_t GetRecordingTransaction();

    uint64_t DetachRecordingTransaction();

    void SetRecordingTransaction(uint64_t transaction);

    void AddWaitSemaphore(VkPipelineStageFlags waitFlags, VulkanSemaphore* pWaitSemaphore)
    {
        AddWaitSemaphore(waitFlags, pWaitSemaphore, 0);
    }

    void AddWaitSemaphore(VkPipelineStageFlags waitFlags, VulkanSemaphore* pWaitSemaphore, uint64_t value)
    {
        VulkanWorkload* pCurrentWorkload = GetWorkload(WorkloadPhase::eWait);
        bool            merged           = false;
        if (value != 0)
        {
            for (VulkanWorkload::WaitSemaphoreInfo& wait : pCurrentWorkload->m_waitSemaphoreInfos)
            {
                if (wait.pSemaphore == pWaitSemaphore)
                {
                    wait.value      = std::max(wait.value, value);
                    wait.waitFlags |= waitFlags;
                    merged          = true;
                    break;
                }
            }
        }
        if (!merged)
        {
            pCurrentWorkload->m_waitSemaphoreInfos.emplace_back(
                VulkanWorkload::WaitSemaphoreInfo{waitFlags, pWaitSemaphore, value});
        }
    }

    void AddSignalSemaphore(VulkanSemaphore* pSignalSemaphore)
    {
        AddSignalSemaphore(pSignalSemaphore, 0);
    }

    void AddSignalSemaphore(VulkanSemaphore* pSignalSemaphore, uint64_t value)
    {
        VulkanWorkload* pCurrentWorkload = GetWorkload(WorkloadPhase::eSignal);
        pCurrentWorkload->m_signalSemaphoreInfos.emplace_back(VulkanWorkload::SignalSemaphoreInfo{pSignalSemaphore, value});
    }


    void RecordLifetime(uint64_t id);

    // Native callers declare every wrapper used by their external Vulkan commands.
    void RecordResource(RHIResource* resource);

    uint64_t GetWorkloadGeneration() const
    {
        return m_pCurrentWorkload != nullptr ? m_workloadGeneration : 0;
    }

    void RecordUniformBufferBlock(uint64_t blockId);

    // Transfer staged workloads and their bindless reservations. A platform batch can
    // retain transaction ownership until its aggregate submission result is known.
    void CollectWorkloads(HeapVector<VulkanWorkload*>& outWorkloads, uint64_t* transactionOwner = nullptr);

    // Finalize the current workload and submit all staged workloads immediately.
    RHISubmissionResult SubmitRecordedWorkloads();

    VulkanQueue* GetQueue() const
    {
        return m_pQueue;
    }

private:
    enum class WorkloadPhase : uint8_t
    {
        eWait,
        eExecute,
        eSignal,
    };

    VulkanWorkload* GetWorkload(WorkloadPhase phase);

    bool HasWorkloadData(const VulkanWorkload* pWorkload) const;

    void FinalizePendingWorkload();

    void SetupNewCommandBuffer();

    void StartWorkload();

    void EndWorkload();

    VulkanQueue* m_pQueue{nullptr};

    VulkanCommandBufferPool* m_pCmdBufferPool{nullptr};

    VulkanWorkload*             m_pCurrentWorkload{nullptr};
    HeapVector<VulkanWorkload*> m_finalizedWorkloads;
    WorkloadPhase               m_currentWorkloadPhase{WorkloadPhase::eWait};
    uint64_t                    m_workloadGeneration{0};

    RHIError m_recordingError{};

    uint64_t m_recordingTransaction{0};
};

// Pipeline and descriptor bindings for one bind point.
class VulkanBindPointState
{
public:
    explicit VulkanBindPointState(VulkanRHI* pRHI) : m_descriptorSetState(pRHI) {}

    void SetPipelineState(RHIPipeline* pPipeline);

    bool SetShaderParameters(RHIShaderParameterView parameters, uint64_t recordedEpoch = 0, uint64_t transaction = 0);

    // Binds the pipeline and its descriptor sets. Returns the command buffer, or null after latching an error.
    VulkanCommandBuffer* BindForCommand(VulkanCommandListContext* pContext);

protected:
    VulkanPipeline* m_pCurrentPipeline{nullptr};

private:
    HeapVector<VkDescriptorSet> m_descriptorSets;
    HeapVector<uint32_t>        m_dynamicOffsets;
    VulkanDescriptorSetState    m_descriptorSetState;
};

class VulkanGfxState : public VulkanBindPointState
{
public:
    using VulkanBindPointState::VulkanBindPointState;

    void SetViewport(uint32_t minX, uint32_t minY, uint32_t maxX, uint32_t maxY);

    void SetScissor(uint32_t minX, uint32_t minY, uint32_t maxX, uint32_t maxY);

    void SetBlendConstants(float r, float g, float b, float a);

    void SetLineWidth(float lineWidth);

    void SetDepthBias(float depthBiasConstantFactor, float depthBiasClamp, float depthBiasSlopeFactor);

    void SetVertexBuffers(uint32_t numVertexBuffers, RHIBuffer* const* ppVertexBuffers, const uint64_t* pOffsets);

    void ClearVertexBuffers();

    bool PreDraw(VulkanCommandListContext* pContext);

private:
    HeapVector<VkViewport> m_viewports;
    HeapVector<VkRect2D>   m_scissors;

    HeapVector<VkBuffer> m_vertexBuffers;
    HeapVector<uint64_t> m_vertexBufferOffsets;

    struct RasterizationStates
    {
        float depthBiasConstantFactor{0.0f};
        float depthBiasClamp{0.0f};
        float depthBiasSlopeFactor{0.0f};
        float lineWidth{1.0f};
    };

    RasterizationStates m_rasterizationState{};

    float m_blendConstants[4]{};
};

class VulkanCommandListContext : public IRHICommandContext, public VulkanCommandContextBase
{
public:
    VulkanCommandListContext(RHICommandContextType contextType, VulkanDevice* pDevice);

    RHICommandContextType GetContextType() override;

    RHIError RHIGetRecordingError() const override
    {
        return GetRecordingError();
    }

    void RHIDiscardRecording() override;

    void DiscardRecording() override;

    void ResetShaderBindings();

    void RHIBeginRendering(const RHIRenderingLayout* pRenderingLayout) override;

    void RHIEndRendering() override;

    void RHIBeginDebugLabel(std::string_view name) override;

    void RHIEndDebugLabel() override;

    void RHIBeginGPUTiming(const RHIGPUTimingPtr& result) override;

    void RHIEndGPUTiming(const RHIGPUTimingPtr& result) override;

    void RHISetScissor(uint32_t minX, uint32_t minY, uint32_t maxX, uint32_t maxY) override;

    void RHISetViewport(uint32_t minX, uint32_t minY, uint32_t maxX, uint32_t maxY) override;

    void RHISetDepthBias(float depthBiasConstantFactor, float depthBiasClamp, float depthBiasSlopeFactor) override;

    void RHISetLineWidth(float lineWidth) override;

    void RHISetBlendConstants(const Color& blendConstants) override;

    void RHIBindPipeline(RHIPipeline* pPipeline) override;

    void RHISetShaderParameters(RHIShaderParameterView parameters) override;

    uint64_t RHIGetCurrentBindlessEpoch() const override;

    uint64_t RHICaptureBindlessEpoch() override;

    void RHIReleaseBindlessEpoch(uint64_t epoch) override;

    void RHISetRecordedBindlessEpoch(uint64_t epoch) override
    {
        m_recordedBindlessEpoch = epoch;
    }

    void RecordCurrentBindlessEpoch();

    void RHIBindVertexBuffers(std::span<RHIBuffer*> pBuffers, std::span<uint64_t> offsets) override;

    void RHIBindVertexBuffer(RHIBuffer* pBuffer, uint64_t offset) override;

    void RHIDraw(uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex, uint32_t firstInstance) override;

    void RHIDrawIndexed(RHIBuffer* pIndexBuffer,
                        DataFormat indexFormat,
                        uint64_t   indexBufferOffset,
                        uint32_t   indexCount,
                        uint32_t   instanceCount,
                        uint32_t   firstIndex,
                        int32_t    vertexOffset,
                        uint32_t   firstInstance) override;

    void RHIDrawIndexedIndirect(RHIBuffer* pIndirectBuffer,
                                RHIBuffer* pIndexBuffer,
                                DataFormat indexFormat,
                                uint64_t   indexBufferOffset,
                                uint64_t   offset,
                                uint32_t   drawCount,
                                uint32_t   stride) override;

    void RHIDispatch(uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ) override;

    void RHIDispatchIndirect(RHIBuffer* pIndirectBuffer, uint64_t offset) override;

    void RHITraceRays(uint32_t width, uint32_t height, uint32_t depth) override;

    void RHITraceRaysIndirect(RHIBuffer* pIndirectBuffer, uint64_t offset) override;

    void RHISetPushConstants(RHIPipeline* pPipeline, std::span<const uint8_t> data, uint32_t offset = 0) override;

    void RHIAddTransitions(RHIFlags<RHIPipelineStageFlagBits> srcStages,
                           RHIFlags<RHIPipelineStageFlagBits> dstStages,
                           std::span<RHIMemoryTransition>     memoryTransitions,
                           std::span<RHIBufferTransition>     bufferTransitions,
                           std::span<RHITextureTransition>    textureTransitions) override;

    void RHIClearBuffer(RHIBuffer* pBuffer, uint64_t offset, uint64_t size) override;

    void RHIBuildAccelerationStructure(const RHIAccelerationStructureBuildInfo& info) override;

    void RHICopyBuffer(RHIBuffer* pSrcBuffer, RHIBuffer* pDstBuffer, const RHIBufferCopyRegion& region) override;

    void RHIClearTexture(RHITexture* pTexture, const Color& color, const RHITextureSubresourceRange& range) override;

    void RHICopyTexture(RHITexture* pSrcTexture, RHITexture* pDstTexture, std::span<RHITextureCopyRegion> regions) override;

    void RHIBlitTexture(RHITexture*                     pSrcTexture,
                        RHITexture*                     pDstTexture,
                        std::span<RHITextureBlitRegion> regions,
                        RHISamplerFilter                filter) override;

    void RHICopyTextureToBuffer(RHITexture*                           pSrcTex,
                                RHIBuffer*                            pDstBuffer,
                                std::span<RHIBufferTextureCopyRegion> regions) override;

    void RHICopyBufferToTexture(RHIBuffer*                            pSrcBuffer,
                                RHITexture*                           pDstTexture,
                                std::span<RHIBufferTextureCopyRegion> regions) override;

    void RHIResolveTexture(RHITexture* pSrcTexture,
                           RHITexture* pDstTexture,
                           uint32_t    srcLayer,
                           uint32_t    srcMipmap,
                           uint32_t    dstLayer,
                           uint32_t    dstMipmap) override;

    // todo: need a function to collect recorded workload in this context
private:
    void MakeHostReadAvailable(RHIBuffer* buffer, uint64_t offset, uint64_t size);

    bool PrepareTraceRays();

    RHICommandContextType m_contextType;

    VulkanDevice* m_pDevice{nullptr};

    VulkanPipeline* m_pCurrentPipeline{nullptr};

    uint64_t m_recordedBindlessEpoch{0};

    VulkanGfxState       m_gfxState;
    VulkanBindPointState m_computeState;
    VulkanBindPointState m_rayTracingState;
};

} // namespace zen
