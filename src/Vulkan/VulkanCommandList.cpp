#include "VulkanFaultReport.h"
#include "ZenRHI/RHIDiagnostics.h"
#include <span>
#include "ZenRHI/RHIDeviceDesc.h"
#include "Vulkan/VulkanTexture.h"
#include "Vulkan/VulkanBuffer.h"
#include "Vulkan/VulkanUniformRing.h"
#include "Vulkan/VulkanPipeline.h"
#include "Vulkan/VulkanCommandList.h"
#include "Vulkan/VulkanCommon.h"
#include "Vulkan/VulkanDevice.h"
#include "Vulkan/VulkanDescriptorAllocator.h"
#include "Vulkan/VulkanBindlessHeap.h"
#include "Vulkan/VulkanDescriptorState.h"
#include "Vulkan/VulkanQueue.h"
#include "Vulkan/VulkanRHI.h"
#include "Vulkan/VulkanSynchronization.h"
#include "Vulkan/VulkanTypes.h"
#include <chrono>
#include "ZenFoundation/Templates/HeapVector.h"
#include "ZenFoundation/Utils/Errors.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>

namespace zen
{
template <typename T> static bool SameValues(const HeapVector<T>& left, const HeapVector<T>& right)
{
    return left.size() == right.size() && std::equal(left.begin(), left.end(), right.begin());
}

void VulkanCommandBuffer::InvalidateCachedState()
{
    for (BoundPipelineState& state : m_boundStates)
    {
        state.pipeline         = VK_NULL_HANDLE;

        state.descriptorLayout = VK_NULL_HANDLE;

        state.firstSet         = 0;

        state.descriptorSets.clear();

        state.dynamicOffsets.clear();
    }

    m_validDynamicStates.Reset();

    m_boundVertexBuffers.clear();

    m_boundVertexOffsets.clear();

    m_boundIndexBuffer = VK_NULL_HANDLE;
}

void VulkanCommandBuffer::BindIndexBuffer(VkBuffer buffer, uint64_t offset, VkIndexType type)
{
    if (m_boundIndexBuffer != buffer || m_boundIndexOffset != offset || m_boundIndexType != type)
    {
        vkCmdBindIndexBuffer(m_vkHandle, buffer, offset, type);

        m_boundIndexBuffer = buffer;

        m_boundIndexOffset = offset;

        m_boundIndexType   = type;
    }
}

void VulkanCommandBuffer::BindPipelineAndDescriptorSets(VulkanPipeline*                    pipeline,
                                                        const HeapVector<VkDescriptorSet>& sets,
                                                        uint32_t                           firstSet,
                                                        const HeapVector<uint32_t>&        offsets)
{
    if (pipeline == nullptr)
    {
        m_error = {RHIErrorCode::eInvalidArgument, 0, "Bind pipeline and descriptors"};
    }
    else if (!m_error.IsFailure())
    {
        const VkPipelineBindPoint bindPoint = pipeline->GetVkPipelineBindPoint();

        const uint32_t      stateIndex      = bindPoint == VK_PIPELINE_BIND_POINT_GRAPHICS ? 0
                                            : bindPoint == VK_PIPELINE_BIND_POINT_COMPUTE  ? 1
                                                                                           : 2;
        BoundPipelineState& state           = m_boundStates[stateIndex];

        if (state.pipeline != pipeline->GetVkPipeline())
        {
            vkCmdBindPipeline(m_vkHandle, bindPoint, pipeline->GetVkPipeline());

            state.pipeline = pipeline->GetVkPipeline();

            if (bindPoint == VK_PIPELINE_BIND_POINT_GRAPHICS)
            {
                // A static pipeline can invalidate dynamic values. Conservatively re-emit
                // the next pipeline's dynamic state after every actual graphics pipeline bind.
                m_validDynamicStates.Reset();
            }
        }

        const VkPipelineLayout layout = pipeline->GetVkPipelineLayout();

        if (!sets.empty()
            && (state.descriptorLayout != layout || state.firstSet != firstSet || !SameValues(state.descriptorSets, sets)
                || !SameValues(state.dynamicOffsets, offsets)))
        {
            vkCmdBindDescriptorSets(m_vkHandle, bindPoint, layout, firstSet, static_cast<uint32_t>(sets.size()), sets.data(),
                                    static_cast<uint32_t>(offsets.size()), offsets.empty() ? nullptr : offsets.data());

            state.descriptorLayout = layout;

            state.firstSet         = firstSet;

            state.descriptorSets   = sets;

            state.dynamicOffsets   = offsets;
        }
    }
}

void VulkanCommandBuffer::SetViewport(const VkViewport& viewport)
{
    const uint32_t bit = ToUnderlying(RHIDynamicState::eViewport);

    if (!m_validDynamicStates.Test(bit) || m_viewport.x != viewport.x || m_viewport.y != viewport.y
        || m_viewport.width != viewport.width || m_viewport.height != viewport.height
        || m_viewport.minDepth != viewport.minDepth || m_viewport.maxDepth != viewport.maxDepth)
    {
        vkCmdSetViewport(m_vkHandle, 0, 1, &viewport);

        m_viewport = viewport;

        m_validDynamicStates.Set(bit);
    }
}

void VulkanCommandBuffer::SetScissor(const VkRect2D& scissor)
{
    const uint32_t bit = ToUnderlying(RHIDynamicState::eScissor);

    if (!m_validDynamicStates.Test(bit) || m_scissor.offset.x != scissor.offset.x || m_scissor.offset.y != scissor.offset.y
        || m_scissor.extent.width != scissor.extent.width || m_scissor.extent.height != scissor.extent.height)
    {
        vkCmdSetScissor(m_vkHandle, 0, 1, &scissor);

        m_scissor = scissor;

        m_validDynamicStates.Set(bit);
    }
}

void VulkanCommandBuffer::SetDepthBias(float constantFactor, float clamp, float slopeFactor)
{
    const uint32_t bit = ToUnderlying(RHIDynamicState::eDepthBias);

    if (!m_validDynamicStates.Test(bit) || m_depthBias[0] != constantFactor || m_depthBias[1] != clamp
        || m_depthBias[2] != slopeFactor)
    {
        vkCmdSetDepthBias(m_vkHandle, constantFactor, clamp, slopeFactor);

        m_depthBias[0] = constantFactor;

        m_depthBias[1] = clamp;

        m_depthBias[2] = slopeFactor;

        m_validDynamicStates.Set(bit);
    }
}

void VulkanCommandBuffer::SetLineWidth(float width)
{
    const uint32_t bit = ToUnderlying(RHIDynamicState::eLineWidth);

    if (!m_validDynamicStates.Test(bit) || m_lineWidth != width)
    {
        vkCmdSetLineWidth(m_vkHandle, width);

        m_lineWidth = width;

        m_validDynamicStates.Set(bit);
    }
}

void VulkanCommandBuffer::BindVertexBuffers(const HeapVector<VkBuffer>& buffers, const HeapVector<uint64_t>& offsets)
{
    if (!buffers.empty() && (!SameValues(m_boundVertexBuffers, buffers) || !SameValues(m_boundVertexOffsets, offsets)))
    {
        vkCmdBindVertexBuffers(m_vkHandle, 0, static_cast<uint32_t>(buffers.size()), buffers.data(), offsets.data());

        m_boundVertexBuffers = buffers;

        m_boundVertexOffsets = offsets;
    }
}

void VulkanCommandBuffer::SetBlendConstants(const float* values)
{
    const uint32_t bit = ToUnderlying(RHIDynamicState::eBlendConstants);
    if (!m_validDynamicStates.Test(bit) || !std::equal(m_blendConstants.begin(), m_blendConstants.end(), values))
    {
        vkCmdSetBlendConstants(m_vkHandle, values);
        std::copy_n(values, m_blendConstants.size(), m_blendConstants.begin());
        m_validDynamicStates.Set(bit);
    }
}

bool VulkanCommandBuffer::Begin()
{
    m_timing.Discard(*m_pDevice);

    InvalidateCachedState();

    m_breadcrumbs.Reset();

    VkResult result = VK_SUCCESS;

    if (!m_error.IsFailure() && m_state == State::eNeedReset)
    {
        result = vkResetCommandBuffer(m_vkHandle, 0);

        if (result == VK_SUCCESS)
        {
            m_state = State::eReadyForBegin;
        }
        else
        {
            m_pDevice->GetRHI()->CheckDeviceLoss(result, "vkResetCommandBuffer");

            m_error = MakeVulkanError(result, "vkResetCommandBuffer", __FILE__, __LINE__);
        }
    }

    if (!m_error.IsFailure() && m_state == State::eReadyForBegin)
    {
        VkCommandBufferBeginInfo beginInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};

        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

        result          = vkBeginCommandBuffer(m_vkHandle, &beginInfo);

        if (result == VK_SUCCESS)
        {
            m_state = State::eIsInsideBegin;

            m_timing.BeginRecording(*this);
        }
        else
        {
            m_pDevice->GetRHI()->CheckDeviceLoss(result, "vkBeginCommandBuffer");

            m_error = MakeVulkanError(result, "vkBeginCommandBuffer", __FILE__, __LINE__);
        }
    }

    return !m_error.IsFailure() && HasBegun();
}

bool VulkanCommandBuffer::End()
{
    if (!m_error.IsFailure() && IsOutsideRenderPass())
    {
        m_timing.EndRecording(*this);

        m_breadcrumbs.RecordHostVisibility(m_vkHandle);

        const VkResult result = vkEndCommandBuffer(m_vkHandle);

        if (result == VK_SUCCESS)
        {
            m_state = State::eHasEnded;
        }
        else
        {
            m_pDevice->GetRHI()->CheckDeviceLoss(result, "vkEndCommandBuffer");

            m_error = MakeVulkanError(result, "vkEndCommandBuffer", __FILE__, __LINE__);
        }
    }

    return !m_error.IsFailure() && HasEnded();
}

void VulkanCommandBuffer::BeginRendering(const VkRenderingInfo* pRenderingInfo)
{
    vkCmdBeginRenderingKHR(m_vkHandle, pRenderingInfo);

    m_state = State::eIsInsideRenderPass;
}

void VulkanCommandBuffer::EndRendering()
{
    vkCmdEndRenderingKHR(m_vkHandle);

    m_state = State::eIsInsideBegin;
}

void VulkanCommandBuffer::SetSubmitted()
{
    LockAuto lock(m_pCmdBufferPool->GetMutex());

    m_timing.CloseFrameRecording(*m_pDevice);

    m_state      = State::eSubmitted;

    m_submitTime = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

void VulkanCommandBuffer::SetCompleted()
{
    LockAuto lock(m_pCmdBufferPool->GetMutex());

    if (m_state == State::eSubmitted)
    {
        m_timing.Resolve(*m_pDevice);

        m_state = State::eNeedReset;
    }
}

void VulkanCommandBuffer::Discard()
{
    LockAuto lock(m_pCmdBufferPool->GetMutex());

    ZEN_RHI_VERIFY_MSG(m_state != State::eSubmitted, "Cannot discard a submitted command buffer");

    m_timing.Discard(*m_pDevice);

    InvalidateCachedState();

    if (m_vkHandle != VK_NULL_HANDLE)
    {
        m_state = State::eNeedReset;
    }

    m_error = {};
}

VulkanCommandBuffer::VulkanCommandBuffer(VulkanCommandBufferPool* pPool) :
    m_pCmdBufferPool(pPool), m_pDevice(pPool->GetQueue()->GetDevice())
{
    AllocMemory();
}

VulkanCommandBuffer::~VulkanCommandBuffer()
{
    m_breadcrumbs.Release(*this);

    if (m_state != State::eNotAllocated)
    {
        FreeMemory();
    }
}

void VulkanCommandBuffer::AllocMemory()
{
    VkCommandBufferAllocateInfo allocInfo;

    InitVkStruct(allocInfo, VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO);

    allocInfo.commandPool        = m_pCmdBufferPool->GetVkHandle();

    allocInfo.commandBufferCount = 1;

    allocInfo.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;

    const VkResult result        = allocInfo.commandPool != VK_NULL_HANDLE
                                     ? vkAllocateCommandBuffers(m_pDevice->GetVkHandle(), &allocInfo, &m_vkHandle)
                                     : VK_ERROR_INITIALIZATION_FAILED;

    if (result == VK_SUCCESS && m_vkHandle != VK_NULL_HANDLE)
    {
        m_state = State::eReadyForBegin;

        m_error = {};
    }
    else
    {
        m_pDevice->GetRHI()->CheckDeviceLoss(result, "vkAllocateCommandBuffers");

        m_error = MakeVulkanError(result, "vkAllocateCommandBuffers", __FILE__, __LINE__);
    }
}

void VulkanCommandBuffer::FreeMemory()
{
    m_timing.Discard(*m_pDevice);

    vkFreeCommandBuffers(m_pDevice->GetVkHandle(), m_pCmdBufferPool->GetVkHandle(), 1, &m_vkHandle);

    m_state    = State::eNotAllocated;

    m_vkHandle = VK_NULL_HANDLE;
}

VulkanCommandBufferPool::VulkanCommandBufferPool(VulkanQueue* pQueue) : m_pQueue(pQueue)
{
    VkCommandPoolCreateInfo cmdPoolCI;

    InitVkStruct(cmdPoolCI, VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO);

    cmdPoolCI.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT; // reset cmd buffer

    cmdPoolCI.queueFamilyIndex = pQueue->GetFamilyIndex();

    const VkResult result      = vkCreateCommandPool(m_pQueue->GetDevice()->GetVkHandle(), &cmdPoolCI, nullptr, &m_vkHandle);

    if (result != VK_SUCCESS)
    {
        ZEN_RHI_LOG_ERROR("vkCreateCommandPool failed: {}", GetResultString(result));
    }
}

VulkanCommandBufferPool::~VulkanCommandBufferPool()
{
    for (uint32_t i = 0; i < m_cmdBuffersInUse.size(); i++)
    {
        VulkanCommandBuffer* pCmdBuffer = m_cmdBuffersInUse[i];

        pCmdBuffer->~VulkanCommandBuffer();

        ZEN_MEM_FREE(pCmdBuffer);
    }

    for (uint32_t i = 0; i < m_cmdBuffersFree.size(); i++)
    {
        VulkanCommandBuffer* pCmdBuffer = m_cmdBuffersFree[i];

        pCmdBuffer->~VulkanCommandBuffer();

        ZEN_MEM_FREE(pCmdBuffer);
    }

    vkDestroyCommandPool(m_pQueue->GetDevice()->GetVkHandle(), m_vkHandle, nullptr);
}

void VulkanCommandBufferPool::FreeUnusedCommandBuffers()
{
    LockAuto lock(&m_mutex);

    const double currentTime = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();

    HeapVector<VulkanCommandBuffer*>::iterator it = m_cmdBuffersInUse.end();

    while (it != m_cmdBuffersInUse.begin())
    {
        --it;

        VulkanCommandBuffer* pCmdBuffer = *it;

        if ((pCmdBuffer->m_state == VulkanCommandBuffer::State::eReadyForBegin
             || pCmdBuffer->m_state == VulkanCommandBuffer::State::eNeedReset)
            && (currentTime - pCmdBuffer->m_submitTime) > 10.0f)
        {
            pCmdBuffer->FreeMemory();

            it = m_cmdBuffersInUse.erase(it);

            m_cmdBuffersFree.push_back(pCmdBuffer);
        }
    }
}

VulkanCommandBuffer* VulkanCommandBufferPool::CreateCmdBuffer()
{
    VulkanCommandBuffer* result{};

    if (!m_cmdBuffersFree.empty())
    {
        VulkanCommandBuffer* pCmdBuffer = m_cmdBuffersFree[0];

        m_cmdBuffersFree.remove(0);

        pCmdBuffer->AllocMemory();

        m_cmdBuffersInUse.emplace_back(pCmdBuffer);

        result = pCmdBuffer;
    }
    else
    {
        VulkanCommandBuffer* pCmdBuffer = static_cast<VulkanCommandBuffer*>(ZEN_MEM_ALLOC(sizeof(VulkanCommandBuffer)));

        new (pCmdBuffer) VulkanCommandBuffer(this);

        m_cmdBuffersInUse.emplace_back(pCmdBuffer);

        result = pCmdBuffer;
    }

    return result;
}

VulkanWorkload::~VulkanWorkload() {}

void VulkanWorkload::Ownership::Accept(VulkanRHI& rhi, const VulkanQueue* pQueue, uint64_t serial)
{
    if (ownsBindlessTransaction)
    {
        rhi.GetBindlessHeap()->ResolveTransaction(bindlessTransaction, true);

        ownsBindlessTransaction = false;
    }

    rhi.GetLifetimeTracker().SubmitRecordings(lifetimeIds, pQueue, serial);

    lifetimeIds.clear();
}

void VulkanWorkload::Ownership::Release(VulkanRHI& rhi)
{
    rhi.GetLifetimeTracker().ReleaseRecordings(lifetimeIds);

    lifetimeIds.clear();

    resources.Reset();

    if (ownsBindlessTransaction)
    {
        rhi.GetBindlessHeap()->ReleaseTransferredTransaction(bindlessTransaction);
    }

    bindlessTransaction     = 0;

    ownsBindlessTransaction = false;
}

VulkanCommandBuffer* VulkanWorkload::GetLastCommandBuffer() const
{
    return m_commandBuffers.empty() ? nullptr : m_commandBuffers.back();
}

void VulkanWorkload::AddCommandBuffer(VulkanCommandBuffer* pCmdBuffer)
{
    ZEN_RHI_VERIFY(pCmdBuffer != nullptr);

    m_commandBuffers.push_back(pCmdBuffer);
}

void VulkanWorkload::Merge(VulkanWorkload* pOtherWorkload)
{
    ZEN_RHI_VERIFY(pOtherWorkload != nullptr);

    ZEN_RHI_VERIFY(pOtherWorkload->m_pQueue == m_pQueue);

    ZEN_RHI_VERIFY(m_signalSemaphoreInfos.empty());

    ZEN_RHI_VERIFY(pOtherWorkload->m_waitSemaphoreInfos.empty());

    m_commandBuffers.push_back(pOtherWorkload->m_commandBuffers);

    pOtherWorkload->m_commandBuffers.clear();

    m_signalSemaphoreInfos.push_back(pOtherWorkload->m_signalSemaphoreInfos);

    pOtherWorkload->m_signalSemaphoreInfos.clear();

    // Keep duplicate IDs: each source recording contributed its own pending count.
    m_ownership.lifetimeIds.push_back(pOtherWorkload->m_ownership.lifetimeIds);

    pOtherWorkload->m_ownership.lifetimeIds.clear();

    m_submissionBoundary          = pOtherWorkload->m_submissionBoundary;

    pOtherWorkload->m_pMergedInto = this;

    m_mergedWorkloads.push_back(pOtherWorkload);
}

VulkanCommandContextBase::~VulkanCommandContextBase()
{
    // CollectWorkloads transfers ownership out of these lists before queue submission.
    VulkanCommandContextBase::DiscardRecording();

    m_pQueue->RecycleCommandBufferPool(m_pCmdBufferPool);
}

bool VulkanCommandContextBase::HasWorkloadData(const VulkanWorkload* pWorkload) const
{
    return pWorkload != nullptr
        && (pWorkload->HasCommandBuffers() || !pWorkload->m_waitSemaphoreInfos.empty()
            || !pWorkload->m_signalSemaphoreInfos.empty() || !pWorkload->m_ownership.lifetimeIds.empty()
            || pWorkload->m_ownership.resources.GetCount() != 0);
}

VulkanWorkload* VulkanCommandContextBase::GetWorkload(WorkloadPhase phase)
{
    if (m_pCurrentWorkload != nullptr && phase < m_currentWorkloadPhase)
    {
        FinalizePendingWorkload();
    }

    if (m_pCurrentWorkload == nullptr)
    {
        StartWorkload();
    }

    m_currentWorkloadPhase = phase;

    return m_pCurrentWorkload;
}

void VulkanCommandContextBase::CollectWorkloads(HeapVector<VulkanWorkload*>& outWorkloads, uint64_t* transactionOwner)
{
    FinalizePendingWorkload();

    const uint64_t      transaction = DetachRecordingTransaction();
    VulkanBindlessHeap* manager     = m_pQueue->GetDevice()->GetRHI()->GetBindlessHeap();
    const bool          rawOwners   = transactionOwner == nullptr && transaction != 0
                                   && manager->TransferTransaction(transaction, m_finalizedWorkloads.size());
    if (transactionOwner != nullptr)
    {
        *transactionOwner = transaction;
    }
    if (!m_finalizedWorkloads.empty())
    {
        for (VulkanWorkload* workload : m_finalizedWorkloads)
        {
            workload->m_ownership.bindlessTransaction     = transaction;
            workload->m_ownership.ownsBindlessTransaction = rawOwners;
        }
        outWorkloads.push_back(m_finalizedWorkloads);

        m_finalizedWorkloads.clear();
    }
}

void VulkanCommandContextBase::FinalizePendingWorkload()
{
    if (m_pCurrentWorkload != nullptr)
    {
        ZEN_RHI_VERIFY(m_pCurrentWorkload->m_pQueue == m_pQueue);

        if (HasWorkloadData(m_pCurrentWorkload))
        {
            EndWorkload();

            m_finalizedWorkloads.push_back(m_pCurrentWorkload);
        }
        else
        {
            m_pQueue->ReleaseWorkload(m_pCurrentWorkload);
        }

        m_pCurrentWorkload     = nullptr;

        m_currentWorkloadPhase = WorkloadPhase::eWait;
    }
}

RHISubmissionResult VulkanCommandContextBase::SubmitRecordedWorkloads()
{
    m_pQueue->GetDevice()->GetRHI()->SetSubmissionError({});
    ZEN_RHI_VERIFY_MSG(!m_pQueue->HasPendingSubmissions(), "Immediate submission cannot interleave with a pending batch");
    RHISubmissionResult result = RHISubmissionResult::eSuccess;

    HeapVector<VulkanWorkload*> workloads;

    uint64_t transaction = 0;
    CollectWorkloads(workloads, &transaction);

    if (m_recordingError.IsFailure())
    {
        m_pQueue->GetDevice()->GetRHI()->SetSubmissionError(m_recordingError);
        result = m_recordingError.code == RHIErrorCode::eDeviceLost || m_pQueue->GetDevice()->GetRHI()->AreSubmissionsBlocked()
                   ? RHISubmissionResult::eFatal
                   : RHISubmissionResult::eRejected;

        for (VulkanWorkload* workload : workloads)
        {
            m_pQueue->DiscardWorkload(workload);
        }

        DiscardRecording();
    }
    else
    {
        for (VulkanWorkload* workload : workloads)
        {
            m_pQueue->EnqueueWorkload(workload);
        }

        if (!workloads.empty())
        {
            uint64_t serial = 0;

            result = m_pQueue->GetDevice()->GetRHI()->AreSubmissionsBlocked() ? RHISubmissionResult::eFatal
                                                                              : m_pQueue->SubmitPendingWorkloads(serial);

            if (result == RHISubmissionResult::eFatal)
            {
                m_pQueue->DiscardPendingWorkloads(true);
                m_pQueue->GetDevice()->GetRHI()->BlockSubmissions(m_pQueue->GetDevice()->GetRHI()->GetSubmissionFailureCause());
            }
            else if (result == RHISubmissionResult::eRejected)
            {
                m_pQueue->DiscardPendingWorkloads(false);
            }
            else
            {
                m_pQueue->ProcessPendingWorkloads(0);
            }
        }
        else if (m_pQueue->GetDevice()->GetRHI()->AreSubmissionsBlocked())
        {
            result = RHISubmissionResult::eFatal;
        }
    }

    m_pQueue->GetDevice()->GetRHI()->GetBindlessHeap()->ResolveTransaction(
        transaction, result == RHISubmissionResult::eSuccess || result == RHISubmissionResult::eFatal);

    return result;
}

void VulkanCommandContextBase::RecordLifetime(uint64_t id)
{
    if (id != 0)
    {
        HeapVector<uint64_t>& ids = GetWorkload(WorkloadPhase::eExecute)->m_ownership.lifetimeIds;

        if ((ids.empty() || ids.back() != id) && std::find(ids.begin(), ids.end(), id) == ids.end())
        {
            ids.push_back(id);

            m_pQueue->GetDevice()->GetRHI()->GetLifetimeTracker().RetainRecording(id);
        }
    }
}

void VulkanCommandContextBase::RecordResource(RHIResource* resource)
{
    if (resource != nullptr && !m_recordingError.IsFailure())
    {
        GetWorkload(WorkloadPhase::eExecute)->m_ownership.resources.Retain(resource);
    }
}

void VulkanCommandContextBase::LatchError(RHIError error)
{
    if (error.IsFailure())
    {
        if (!m_recordingError.IsFailure())
        {
            m_recordingError = error;

            ZEN_RHI_LOG_ERROR("RHI recording failed in {} (native {})", RHIErrorText(error.operation, "unknown operation"),
                              error.nativeCode);
        }

        if (error.code == RHIErrorCode::eDeviceLost)
        {
            m_pQueue->GetDevice()->GetRHI()->BlockSubmissions(error);
        }
    }
}

bool VulkanCommandContextBase::EnsureRecording()
{
    if (!m_recordingError.IsFailure())
    {
        VulkanCommandBuffer* buffer = GetCommandBuffer();

        LatchError(buffer->GetError());
    }

    return !m_recordingError.IsFailure();
}

void VulkanCommandContextBase::DiscardRecording()
{
    if (m_pCurrentWorkload != nullptr)
    {
        m_pQueue->DiscardWorkload(m_pCurrentWorkload);

        m_pCurrentWorkload = nullptr;
    }

    for (VulkanWorkload* workload : m_finalizedWorkloads)
    {
        m_pQueue->DiscardWorkload(workload);
    }

    m_finalizedWorkloads.clear();

    m_pQueue->GetDevice()->GetRHI()->GetBindlessHeap()->ResolveTransaction(m_recordingTransaction, false);

    m_recordingTransaction = 0;

    m_currentWorkloadPhase = WorkloadPhase::eWait;

    m_recordingError       = {};
}

uint64_t VulkanCommandContextBase::GetRecordingTransaction()
{
    if (m_recordingTransaction == 0)
    {
        m_recordingTransaction = m_pQueue->GetDevice()->GetRHI()->GetBindlessHeap()->BeginTransaction();
    }

    return m_recordingTransaction;
}

uint64_t VulkanCommandContextBase::DetachRecordingTransaction()
{
    const uint64_t transaction = m_recordingTransaction;

    m_recordingTransaction     = 0;

    return transaction;
}

void VulkanCommandContextBase::SetRecordingTransaction(uint64_t transaction)
{
    if (m_recordingTransaction == 0)
    {
        m_recordingTransaction = transaction;
    }
}

void VulkanCommandContextBase::RecordUniformBufferBlock(uint64_t blockId)
{
    RecordLifetime(m_pQueue->GetDevice()->GetRHI()->GetUniformRing()->GetBlockLifetime(blockId));
}

void VulkanCommandContextBase::SetupNewCommandBuffer()
{
    LockAuto lock(&m_pCmdBufferPool->m_mutex);

    VulkanCommandBuffer* pCmdBuffer  = nullptr;
    VulkanCommandBuffer* unallocated = nullptr;
    for (VulkanCommandBuffer* candidate : m_pCmdBufferPool->m_cmdBuffersInUse)
    {
        if (candidate->m_state == VulkanCommandBuffer::State::eReadyForBegin
            || candidate->m_state == VulkanCommandBuffer::State::eNeedReset)
        {
            pCmdBuffer = candidate;
            break;
        }
        if (candidate->m_state == VulkanCommandBuffer::State::eNotAllocated)
        {
            unallocated = candidate;
        }
        else
        {
            ZEN_RHI_VERIFY(candidate->IsSubmitted() || candidate->HasEnded());
        }
    }
    if (pCmdBuffer == nullptr && unallocated != nullptr)
    {
        unallocated->AllocMemory();
        pCmdBuffer = unallocated;
    }

    if (!pCmdBuffer)
    {
        pCmdBuffer = m_pCmdBufferPool->CreateCmdBuffer();
    }

    m_pCurrentWorkload->AddCommandBuffer(pCmdBuffer);

    if (!pCmdBuffer->Begin())
    {
        LatchError(pCmdBuffer->GetError());
    }
}

void VulkanCommandContextBase::StartWorkload()
{
    ZEN_RHI_VERIFY(m_pCurrentWorkload == nullptr);

    ++m_workloadGeneration;

    m_pCurrentWorkload     = m_pQueue->AcquireWorkload();

    m_currentWorkloadPhase = WorkloadPhase::eWait;
}

void VulkanCommandContextBase::EndWorkload()
{
    if (m_pCurrentWorkload != nullptr)
    {
        VulkanCommandBuffer* pCommandBuffer = m_pCurrentWorkload->GetLastCommandBuffer();

        if (pCommandBuffer != nullptr && !pCommandBuffer->HasEnded())
        {
            if (pCommandBuffer->IsInsideRenderPass())
            {
                pCommandBuffer->EndRendering();
            }

            if (!pCommandBuffer->End())
            {
                LatchError(pCommandBuffer->GetError());
            }
        }
    }
}

void VulkanGfxState::SetViewport(uint32_t minX, uint32_t minY, uint32_t maxX, uint32_t maxY)
{
    m_viewports.resize(1);

    m_viewports[0]          = {};

    m_viewports[0].x        = minX;

    m_viewports[0].y        = minY;

    m_viewports[0].width    = maxX - minX;

    m_viewports[0].height   = maxY - minY;

    m_viewports[0].minDepth = 0.0f;

    m_viewports[0].maxDepth = 1.0f;
}

void VulkanGfxState::SetScissor(uint32_t minX, uint32_t minY, uint32_t maxX, uint32_t maxY)
{
    m_scissors.resize(1);

    m_scissors[0].offset.x      = minX;

    m_scissors[0].offset.y      = minY;

    m_scissors[0].extent.width  = maxX - minX;

    m_scissors[0].extent.height = maxY - minY;
}

void VulkanGfxState::SetBlendConstants(float r, float g, float b, float a)
{
    m_blendConstants[0] = r;

    m_blendConstants[1] = g;

    m_blendConstants[2] = b;

    m_blendConstants[3] = a;
}

void VulkanGfxState::SetLineWidth(float lineWidth)
{
    m_rasterizationState.lineWidth = lineWidth;
}

void VulkanGfxState::SetDepthBias(float depthBiasConstantFactor, float depthBiasClamp, float depthBiasSlopeFactor)
{
    m_rasterizationState.depthBiasConstantFactor = depthBiasConstantFactor;

    m_rasterizationState.depthBiasClamp          = depthBiasClamp;

    m_rasterizationState.depthBiasSlopeFactor    = depthBiasSlopeFactor;
}

void VulkanGfxState::SetVertexBuffers(uint32_t numVertexBuffers, RHIBuffer* const* ppVertexBuffers, const uint64_t* pOffsets)
{
    m_vertexBuffers.resize(numVertexBuffers);

    m_vertexBufferOffsets.resize(numVertexBuffers);

    for (uint32_t i = 0; i < numVertexBuffers; i++)
    {
        m_vertexBuffers[i]       = TO_VK_BUFFER(ppVertexBuffers[i])->GetVkBuffer();

        m_vertexBufferOffsets[i] = pOffsets[i];
    }
}

void VulkanGfxState::ClearVertexBuffers()
{
    m_vertexBuffers.clear();

    m_vertexBufferOffsets.clear();
}

void VulkanBindPointState::SetPipelineState(RHIPipeline* pPipeline)
{
    m_pCurrentPipeline = TO_VK_PIPELINE(pPipeline);

    m_descriptorSetState.SetPipeline(m_pCurrentPipeline);
}

bool VulkanBindPointState::SetShaderParameters(RHIShaderParameterView parameters, uint64_t recordedEpoch, uint64_t transaction)
{
    return m_pCurrentPipeline != nullptr && m_descriptorSetState.SetShaderParameters(parameters, recordedEpoch, transaction);
}

VulkanCommandBuffer* VulkanBindPointState::BindForCommand(VulkanCommandListContext* pContext)
{
    VulkanCommandBuffer* result = nullptr;
    RHIStatus preparation{MakeRHIError(RHIErrorCode::eInvalidArgument, "Prepare shader descriptors", __FILE__, __LINE__)};

    if (m_pCurrentPipeline != nullptr && pContext->EnsureRecording())
    {
        VulkanCommandBuffer* commandBuffer = pContext->GetCommandBuffer();

        uint32_t firstSet                  = 0;

        // Always resolve/retain descriptor pools: cache eviction and a new workload can
        // require work even when the native binding commands themselves are unchanged.
        preparation = m_descriptorSetState.FlushPendingDescriptorWrites(pContext, m_descriptorSets, firstSet, m_dynamicOffsets);

        if (static_cast<bool>(preparation))
        {
            commandBuffer->BindPipelineAndDescriptorSets(m_pCurrentPipeline, m_descriptorSets, firstSet, m_dynamicOffsets);

            result = commandBuffer;
        }
    }

    if (result == nullptr)
    {
        pContext->LatchError(preparation.error);
    }

    return result;
}

bool VulkanGfxState::PreDraw(VulkanCommandListContext* pContext)
{
    VulkanCommandBuffer* commandBuffer = BindForCommand(pContext);

    bool ready                         = commandBuffer != nullptr;

    if (ready)
    {
        // Dynamic commands must follow the last static pipeline that invalidated them.
        // Emit only states declared dynamic by the active pipeline.
        if (m_pCurrentPipeline->UsesDynamicState(RHIDynamicState::eViewport) && !m_viewports.empty())
        {
            commandBuffer->SetViewport(m_viewports[0]);
        }

        if (m_pCurrentPipeline->UsesDynamicState(RHIDynamicState::eScissor) && !m_scissors.empty())
        {
            commandBuffer->SetScissor(m_scissors[0]);
        }

        if (m_pCurrentPipeline->UsesDynamicState(RHIDynamicState::eDepthBias))
        {
            commandBuffer->SetDepthBias(m_rasterizationState.depthBiasConstantFactor, m_rasterizationState.depthBiasClamp,
                                        m_rasterizationState.depthBiasSlopeFactor);
        }

        if (m_pCurrentPipeline->UsesDynamicState(RHIDynamicState::eLineWidth))
        {
            commandBuffer->SetLineWidth(m_rasterizationState.lineWidth);
        }

        if (m_pCurrentPipeline->UsesDynamicState(RHIDynamicState::eBlendConstants))
        {
            commandBuffer->SetBlendConstants(m_blendConstants);
        }

        // Procedural draws declare no vertex inputs. Rebinding a previous draw's
        // cached handles would introduce an undeclared use after their owner retires.
        const VulkanShader* shader    = TO_VK_SHADER(m_pCurrentPipeline->GetShader());

        const uint32_t vertexBindings = shader->GetVertexInputStateCreateInfoData()->vertexBindingDescriptionCount;

        if (m_vertexBuffers.size() < vertexBindings)
        {
            pContext->LatchError(MakeRHIError(RHIErrorCode::eInvalidArgument, "Bind vertex buffers", __FILE__, __LINE__));

            ready = false;
        }
        else if (vertexBindings != 0)
        {
            commandBuffer->BindVertexBuffers(m_vertexBuffers, m_vertexBufferOffsets);
        }
    }

    return ready;
}

VulkanCommandListContext::VulkanCommandListContext(RHICommandContextType contextType, VulkanDevice* pDevice) :
    VulkanCommandContextBase(pDevice->GetQueue(contextType)),
    m_contextType(contextType),
    m_pDevice(pDevice),
    m_gfxState(pDevice->GetRHI()),
    m_computeState(pDevice->GetRHI()),
    m_rayTracingState(pDevice->GetRHI())
{
    BindRuntime(&pDevice->GetRHI()->GetRuntime());

    BindRecordingError(&GetRecordingError());
}

void VulkanCommandListContext::RHIDiscardRecording()
{
    DiscardRecording();
}

void VulkanCommandListContext::DiscardRecording()
{
    VulkanCommandContextBase::DiscardRecording();

    ResetShaderBindings();
}

void VulkanCommandListContext::ResetShaderBindings()
{
    m_pCurrentPipeline = nullptr;

    m_gfxState.SetPipelineState(nullptr);

    m_gfxState.ClearVertexBuffers();

    m_computeState.SetPipelineState(nullptr);
    m_rayTracingState.SetPipelineState(nullptr);
}

RHICommandContextType VulkanCommandListContext::GetContextType()
{
    return m_contextType;
}

static RHITextureView* ResolveRenderingAttachment(const RHIRenderTarget&    target,
                                                  const RHIRenderingLayout& layout,
                                                  bool                      depthStencil,
                                                  SampleCount&              samples,
                                                  bool&                     hasSamples)
{
    RHITextureView* view = nullptr;

    bool valid           = target.pTexture != nullptr;

    if (valid)
    {
        view = target.pTextureView != nullptr ? target.pTextureView : TO_VK_TEXTURE(target.pTexture)->GetAttachmentView();

        const bool depthFormat =
            FormatIsDepthOnly(target.format) || FormatIsStencilOnly(target.format) || FormatIsDepthStencil(target.format);

        const RHITextureUsageFlagBits usage =
            depthStencil ? RHITextureUsageFlagBits::eDepthStencilAttachment : RHITextureUsageFlagBits::eColorAttachment;

        valid = view != nullptr && view->GetTexture() == target.pTexture && view->GetFormat() == target.format
             && depthStencil == depthFormat && target.pTexture->GetBaseInfo().usageFlags.HasFlag(usage);

        if (valid)
        {
            const RHITextureSubresourceRange& range = view->GetSubResourceRange();

            const uint32_t width                    = std::max(1u, target.pTexture->GetWidth() >> range.baseMipLevel);

            const uint32_t height                   = std::max(1u, target.pTexture->GetHeight() >> range.baseMipLevel);

            const SampleCount actualSamples         = target.pTexture->GetBaseInfo().samples;

            valid                                   = view->GetTextureType() != RHITextureType::e3D && range.levelCount == 1
                                                   && layout.numLayers <= range.layerCount && uint32_t(layout.renderArea.maxX) <= width
                                                   && uint32_t(layout.renderArea.maxY) <= height && target.numSamples == actualSamples
                                                   && (!hasSamples || samples == actualSamples);

            if (valid)
            {
                samples    = actualSamples;

                hasSamples = true;
            }
        }
    }

    if (!valid)
    {
        view = nullptr;
    }

    return view;
}

void VulkanCommandListContext::RHIBeginRendering(const RHIRenderingLayout* layout)
{
    bool valid = !GetRecordingError().IsFailure() && layout != nullptr;

    VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};

    VkRenderingAttachmentInfo colors[MAX_NUM_COLOR_ATTACHMENTS]{};

    VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};

    SampleCount samples{};

    bool hasSamples = false;

    if (valid)
    {
        const Rect2<int>& area               = layout->renderArea;

        const VkPhysicalDeviceLimits& limits = m_pDevice->GetPhysicalDeviceProperties().limits;

        valid = area.minX >= 0 && area.minY >= 0 && area.maxX > area.minX && area.maxY > area.minY
             && uint32_t(area.maxX) <= limits.maxFramebufferWidth && uint32_t(area.maxY) <= limits.maxFramebufferHeight
             && layout->numLayers > 0 && layout->numLayers <= limits.maxFramebufferLayers
             && layout->numColorRenderTargets <= std::min<uint32_t>(MAX_NUM_COLOR_ATTACHMENTS, limits.maxColorAttachments);

        if (valid)
        {
            rendering.layerCount           = layout->numLayers;

            rendering.renderArea           = {{area.minX, area.minY}, {uint32_t(area.Width()), uint32_t(area.Height())}};

            rendering.colorAttachmentCount = layout->numColorRenderTargets;

            rendering.pColorAttachments    = colors;

            for (uint32_t i = 0; valid && i < layout->numColorRenderTargets; ++i)
            {
                const RHIRenderTarget& target = layout->colorRenderTargets[i];

                RHITextureView* view          = ResolveRenderingAttachment(target, *layout, false, samples, hasSamples);

                valid                         = view != nullptr;

                if (valid)
                {
                    colors[i]                  = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};

                    colors[i].imageView        = TO_VK_TEXTURE_VIEW(view)->GetVkImageView();

                    colors[i].imageLayout      = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

                    colors[i].loadOp           = ToVkAttachmentLoadOp(target.loadOp);

                    colors[i].storeOp          = ToVkAttachmentStoreOp(target.storeOp);

                    colors[i].clearValue.color = ToVkClearColor(target.clearValue.color, view->GetFormat());

                    RecordResource(view);
                }
            }

            if (valid && layout->hasDepthStencilRT)
            {
                const RHIRenderTarget& target = layout->depthStencilRenderTarget;

                RHITextureView* view          = ResolveRenderingAttachment(target, *layout, true, samples, hasSamples);

                valid                         = view != nullptr;

                if (valid)
                {
                    depth.imageView                                  = TO_VK_TEXTURE_VIEW(view)->GetVkImageView();

                    depth.imageLayout                                = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

                    depth.loadOp                                     = ToVkAttachmentLoadOp(target.loadOp);

                    depth.storeOp                                    = ToVkAttachmentStoreOp(target.storeOp);

                    depth.clearValue.depthStencil                    = ToVkClearDepthStencil(target.clearValue);

                    const RHIFlags<RHITextureAspectFlagBits> aspects = target.GetAspects();

                    rendering.pDepthAttachment   = aspects.HasFlag(RHITextureAspectFlagBits::eDepth) ? &depth : nullptr;

                    rendering.pStencilAttachment = aspects.HasFlag(RHITextureAspectFlagBits::eStencil) ? &depth : nullptr;

                    RecordResource(view);
                }
            }
        }
    }

    if (valid && EnsureRecording())
    {
        GetCommandBuffer()->BeginRendering(&rendering);
    }
    else if (!valid)
    {
        LatchError({RHIErrorCode::eInvalidArgument, 0, "Begin rendering attachments"});
    }
}

void VulkanCommandListContext::RHIEndRendering()
{
    if (EnsureRecording())
    {
        GetCommandBuffer()->EndRendering();
    }
}

void VulkanCommandListContext::RHIBeginDebugLabel(std::string_view name)
{
    if (EnsureRecording())
    {
        const NameID labelName(name);
        GetCommandBuffer()->BeginBreadcrumb(labelName);

        if (m_pDevice->GetRHI()->GetInstanceExtensionFlags().hasDebugUtils && vkCmdBeginDebugUtilsLabelEXT != nullptr
            && vkCmdEndDebugUtilsLabelEXT != nullptr)
        {
            VkDebugUtilsLabelEXT label{VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT};

            label.pLabelName = labelName.CStr();

            vkCmdBeginDebugUtilsLabelEXT(GetCommandBuffer()->GetVkHandle(), &label);
        }
    }
}

void VulkanCommandListContext::RHIEndDebugLabel()
{
    if (EnsureRecording())
    {
        GetCommandBuffer()->EndBreadcrumb();

        if (m_pDevice->GetRHI()->GetInstanceExtensionFlags().hasDebugUtils && vkCmdBeginDebugUtilsLabelEXT != nullptr
            && vkCmdEndDebugUtilsLabelEXT != nullptr)
        {
            vkCmdEndDebugUtilsLabelEXT(GetCommandBuffer()->GetVkHandle());
        }
    }
}

void VulkanCommandListContext::RHIBeginGPUTiming(const RHIGPUTimingPtr& result)
{
    if (EnsureRecording())
    {
        if (result != nullptr && result->GetStatus() == RHIGPUTimingStatus::ePending)
        {
            GetCommandBuffer()->BeginGPUTiming(result);
        }
    }
    else if (result != nullptr && result->GetStatus() == RHIGPUTimingStatus::ePending)
    {
        result->Publish(RHIGPUTimingStatus::eDiscarded);
    }
}

void VulkanCommandListContext::RHIEndGPUTiming(const RHIGPUTimingPtr& result)
{
    if (EnsureRecording())
    {
        if (result != nullptr && result->GetStatus() == RHIGPUTimingStatus::ePending)
        {
            GetCommandBuffer()->EndGPUTiming(result);
        }
    }
}

void VulkanCommandListContext::RHISetScissor(uint32_t minX, uint32_t minY, uint32_t maxX, uint32_t maxY)
{
    m_gfxState.SetScissor(minX, minY, maxX, maxY);
}

void VulkanCommandListContext::RHISetViewport(uint32_t minX, uint32_t minY, uint32_t maxX, uint32_t maxY)
{
    m_gfxState.SetViewport(minX, minY, maxX, maxY);
}

void VulkanCommandListContext::RHISetDepthBias(float depthBiasConstantFactor, float depthBiasClamp, float depthBiasSlopeFactor)
{
    if (depthBiasClamp != 0.0f && !m_pDevice->GetPhysicalDeviceFeatures().depthBiasClamp)
    {
        LatchError(MakeRHIError(RHIErrorCode::eUnsupported, "Set depth bias clamp", __FILE__, __LINE__));
    }
    else
    {
        m_gfxState.SetDepthBias(depthBiasConstantFactor, depthBiasClamp, depthBiasSlopeFactor);
    }
}

void VulkanCommandListContext::RHISetLineWidth(float lineWidth)
{
    if (lineWidth != 1.0f && !m_pDevice->GetPhysicalDeviceFeatures().wideLines)
    {
        LatchError(MakeRHIError(RHIErrorCode::eUnsupported, "Set wide lines", __FILE__, __LINE__));
    }
    else
    {
        m_gfxState.SetLineWidth(lineWidth);
    }
}

void VulkanCommandListContext::RHISetBlendConstants(const Color& blendConstants)
{
    m_gfxState.SetBlendConstants(blendConstants.r, blendConstants.g, blendConstants.b, blendConstants.a);
}

void VulkanCommandListContext::RHIBindPipeline(RHIPipeline* pPipeline)
{
    if (pPipeline == nullptr)
    {
        LatchError({RHIErrorCode::eInvalidArgument, 0, "Bind pipeline"});
    }
    else if (!GetRecordingError().IsFailure())
    {
        VulkanPipeline* pVkPipeline = TO_VK_PIPELINE(pPipeline);

        m_pCurrentPipeline          = pVkPipeline;

        if (pVkPipeline->GetVkPipelineBindPoint() == VK_PIPELINE_BIND_POINT_COMPUTE)
        {
            m_computeState.SetPipelineState(pPipeline);
        }
        else if (pVkPipeline->GetVkPipelineBindPoint() == VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR)
        {
            m_rayTracingState.SetPipelineState(pPipeline);
        }
        else
        {
            m_gfxState.SetPipelineState(pPipeline);
        }
    }
}

uint64_t VulkanCommandListContext::RHIGetCurrentBindlessEpoch() const
{
    return m_pDevice->GetRHI()->GetBindlessHeap()->GetCurrentEpoch();
}

uint64_t VulkanCommandListContext::RHICaptureBindlessEpoch()
{
    return m_pDevice->GetRHI()->GetBindlessHeap()->CaptureEpoch();
}

void VulkanCommandListContext::RHIReleaseBindlessEpoch(uint64_t epoch)
{
    m_pDevice->GetRHI()->GetBindlessHeap()->ReleaseEpoch(epoch);
}

void VulkanCommandListContext::RecordCurrentBindlessEpoch()
{
    if (m_recordedBindlessEpoch != 0)
    {
        RecordLifetime(m_recordedBindlessEpoch);
    }
    else
    {
        const uint64_t epoch = RHICaptureBindlessEpoch();

        RecordLifetime(epoch);

        RHIReleaseBindlessEpoch(epoch);
    }
}

void VulkanCommandListContext::RHISetShaderParameters(RHIShaderParameterView parameters)
{
    bool ready = false;

    if (!GetRecordingError().IsFailure() && m_pCurrentPipeline != nullptr)
    {
        const uint64_t transaction = GetRecordingTransaction();

        switch (m_pCurrentPipeline->GetVkPipelineBindPoint())
        {
            case VK_PIPELINE_BIND_POINT_COMPUTE:
                ready = m_computeState.SetShaderParameters(parameters, m_recordedBindlessEpoch, transaction);
                break;
            case VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR:
                ready = m_rayTracingState.SetShaderParameters(parameters, m_recordedBindlessEpoch, transaction);
                break;
            case VK_PIPELINE_BIND_POINT_GRAPHICS:
                ready = m_gfxState.SetShaderParameters(parameters, m_recordedBindlessEpoch, transaction);
                break;
            default: break;
        }
    }

    if (!ready)
    {
        LatchError({RHIErrorCode::eInvalidArgument, 0, "Set shader parameters"});
    }
}

void VulkanCommandListContext::RHIBindVertexBuffers(std::span<RHIBuffer*> pBuffers, std::span<uint64_t> offsets)
{
    m_gfxState.SetVertexBuffers(pBuffers.size(), pBuffers.data(), offsets.data());
}

void VulkanCommandListContext::RHIBindVertexBuffer(RHIBuffer* pBuffer, uint64_t offset)
{
    m_gfxState.SetVertexBuffers(1, &pBuffer, &offset);
}

void VulkanCommandListContext::RHIDraw(uint32_t vertexCount,
                                       uint32_t instanceCount,
                                       uint32_t firstVertex,
                                       uint32_t firstInstance)
{
    if (m_gfxState.PreDraw(this))
    {
        m_pDevice->GetRHI()->GetExecutionCounterStorage().Increment(m_pDevice->GetRHI()->GetExecutionCounterStorage().draws);

        vkCmdDraw(GetCommandBuffer()->GetVkHandle(), vertexCount, instanceCount, firstVertex, firstInstance);
    }
}

void VulkanCommandListContext::RHIDrawIndexed(RHIBuffer* pIndexBuffer,
                                              DataFormat indexFormat,
                                              uint64_t   indexBufferOffset,
                                              uint32_t   indexCount,
                                              uint32_t   instanceCount,
                                              uint32_t   firstIndex,
                                              int32_t    vertexOffset,
                                              uint32_t   firstInstance)
{
    if (m_gfxState.PreDraw(this))
    {
        VulkanCommandBuffer* pCmdBuffer = GetCommandBuffer();

        VulkanBuffer* pVkBuffer         = TO_VK_BUFFER(pIndexBuffer);

        VkIndexType vkIndexType         = indexFormat == DataFormat::eR16UInt ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;

        pCmdBuffer->BindIndexBuffer(pVkBuffer->GetVkBuffer(), indexBufferOffset, vkIndexType);

        m_pDevice->GetRHI()->GetExecutionCounterStorage().Increment(m_pDevice->GetRHI()->GetExecutionCounterStorage().draws);

        vkCmdDrawIndexed(pCmdBuffer->GetVkHandle(), indexCount, instanceCount, firstIndex, vertexOffset, firstInstance);
    }
}

void VulkanCommandListContext::RHIDrawIndexedIndirect(RHIBuffer* pIndirectBuffer,
                                                      RHIBuffer* pIndexBuffer,
                                                      DataFormat indexFormat,
                                                      uint64_t   indexBufferOffset,
                                                      uint64_t   offset,
                                                      uint32_t   drawCount,
                                                      uint32_t   stride)
{
    const VkPhysicalDeviceFeatures& features = m_pDevice->GetPhysicalDeviceFeatures();
    if ((drawCount > 1 && !features.multiDrawIndirect)
        || drawCount > m_pDevice->GetPhysicalDeviceProperties().limits.maxDrawIndirectCount)
    {
        LatchError(MakeRHIError(RHIErrorCode::eUnsupported, "Draw indexed indirect count", __FILE__, __LINE__));
    }
    if (m_gfxState.PreDraw(this))
    {
        VulkanCommandBuffer* pCmdBuffer = GetCommandBuffer();

        VkIndexType vkIndexType         = indexFormat == DataFormat::eR16UInt ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;

        pCmdBuffer->BindIndexBuffer(TO_VK_BUFFER(pIndexBuffer)->GetVkBuffer(), indexBufferOffset, vkIndexType);

        m_pDevice->GetRHI()->GetExecutionCounterStorage().Increment(m_pDevice->GetRHI()->GetExecutionCounterStorage().draws);

        vkCmdDrawIndexedIndirect(pCmdBuffer->GetVkHandle(), TO_VK_BUFFER(pIndirectBuffer)->GetVkBuffer(), offset, drawCount,
                                 stride);
    }
}

void VulkanCommandListContext::RHIDispatch(uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ)
{
    if (m_computeState.BindForCommand(this) != nullptr)
    {
        m_pDevice->GetRHI()->GetExecutionCounterStorage().Increment(
            m_pDevice->GetRHI()->GetExecutionCounterStorage().dispatches);

        vkCmdDispatch(GetCommandBuffer()->GetVkHandle(), groupCountX, groupCountY, groupCountZ);
    }
}

void VulkanCommandListContext::RHIDispatchIndirect(RHIBuffer* pIndirectBuffer, uint64_t offset)
{
    if (m_computeState.BindForCommand(this) != nullptr)
    {
        VulkanBuffer* pVkBuffer = TO_VK_BUFFER(pIndirectBuffer);

        m_pDevice->GetRHI()->GetExecutionCounterStorage().Increment(
            m_pDevice->GetRHI()->GetExecutionCounterStorage().dispatches);

        vkCmdDispatchIndirect(GetCommandBuffer()->GetVkHandle(), pVkBuffer->GetVkBuffer(), offset);
    }
}

bool VulkanCommandListContext::PrepareTraceRays()
{
    const VkQueueFamilyProperties& queue = m_pDevice->GetQueueFamilyProperties(GetQueue()->GetFamilyIndex());
    bool ready = m_pDevice->GetRHI()->QueryGPUInfo().rayTracing.IsUsable() && (queue.queueFlags & VK_QUEUE_COMPUTE_BIT) != 0
              && m_pCurrentPipeline != nullptr && m_pCurrentPipeline->GetType() == RHIPipelineType::eRayTracing
              && EnsureRecording() && GetCommandBuffer()->IsOutsideRenderPass();
    if (ready)
    {
        ready = m_rayTracingState.BindForCommand(this) != nullptr;
    }
    else
    {
        LatchError(
            MakeRHIError(RHIErrorCode::eInvalidArgument, "Trace rays pipeline/queue/rendering state", __FILE__, __LINE__));
    }
    return ready;
}

void VulkanCommandListContext::RHITraceRays(uint32_t width, uint32_t height, uint32_t depth)
{
    const bool valid =
        m_pDevice->GetRHI()->QueryGPUInfo().IsTraceRaysWithinLimits(width, height, depth) && vkCmdTraceRaysKHR != nullptr;
    if (!valid)
    {
        LatchError(MakeRHIError(RHIErrorCode::eInvalidArgument, "Trace rays launch dimensions", __FILE__, __LINE__));
    }
    else if (PrepareTraceRays())
    {
        const std::array<VkStridedDeviceAddressRegionKHR, RHIShaderBindingTableLayout::Count>& table =
            m_pCurrentPipeline->GetShaderBindingTable();
        vkCmdTraceRaysKHR(GetCommandBuffer()->GetVkHandle(), &table[0], &table[1], &table[2], &table[3], width, height, depth);
        RHIExecutionCounterStorage& counters = m_pDevice->GetRHI()->GetExecutionCounterStorage();
        counters.Increment(counters.rayTraces);
    }
}

void VulkanCommandListContext::RHITraceRaysIndirect(RHIBuffer* pIndirectBuffer, uint64_t offset)
{
    const bool valid = m_pDevice->GetRHI()->QueryGPUInfo().rayTracing.indirect && vkCmdTraceRaysIndirectKHR != nullptr
                    && pIndirectBuffer != nullptr && pIndirectBuffer->GetResourceType() == RHIResourceType::eBuffer
                    && pIndirectBuffer->GetUsageFlags().HasFlag(RHIBufferUsageFlagBits::eIndirectBuffer)
                    && pIndirectBuffer->GetDeviceAddress() != 0 && offset % 4 == 0
                    && offset <= pIndirectBuffer->GetRequiredSize()
                    && sizeof(VkTraceRaysIndirectCommandKHR) <= pIndirectBuffer->GetRequiredSize() - offset
                    && (pIndirectBuffer->GetDeviceAddress() + offset) % 4 == 0;
    if (!valid)
    {
        LatchError(MakeRHIError(RHIErrorCode::eInvalidArgument, "Trace rays indirect buffer/capability", __FILE__, __LINE__));
    }
    else if (PrepareTraceRays())
    {
        const std::array<VkStridedDeviceAddressRegionKHR, RHIShaderBindingTableLayout::Count>& table =
            m_pCurrentPipeline->GetShaderBindingTable();
        vkCmdTraceRaysIndirectKHR(GetCommandBuffer()->GetVkHandle(), &table[0], &table[1], &table[2], &table[3],
                                  pIndirectBuffer->GetDeviceAddress() + offset);
        RHIExecutionCounterStorage& counters = m_pDevice->GetRHI()->GetExecutionCounterStorage();
        counters.Increment(counters.rayTraces);
    }
}

void VulkanCommandListContext::RHISetPushConstants(RHIPipeline* pPipeline, std::span<const uint8_t> data, uint32_t offset)
{
    const VulkanPipeline* pipeline = pPipeline != nullptr && pPipeline->GetResourceType() == RHIResourceType::ePipeline
                                       ? TO_VK_PIPELINE(pPipeline)
                                       : nullptr;
    const uint32_t        range    = pipeline != nullptr ? pipeline->GetShader()->GetPushConstantSize() : 0;
    const bool valid = pipeline != nullptr && data.data() != nullptr && !data.empty() && data.size() % 4 == 0 && offset % 4 == 0
                    && offset <= range && data.size() <= range - offset && pipeline->GetPushConstantsStageFlags() != 0;
    if (!valid)
    {
        LatchError(MakeRHIError(RHIErrorCode::eInvalidArgument, "Set push constants range", __FILE__, __LINE__));
    }
    else if (EnsureRecording())
    {
        vkCmdPushConstants(GetCommandBuffer()->GetVkHandle(), pipeline->GetVkPipelineLayout(),
                           pipeline->GetPushConstantsStageFlags(), offset, static_cast<uint32_t>(data.size()), data.data());
    }
}

void VulkanCommandListContext::RHIAddTransitions(RHIFlags<RHIPipelineStageFlagBits> srcStages,
                                                 RHIFlags<RHIPipelineStageFlagBits> dstStages,
                                                 std::span<RHIMemoryTransition>     memoryTransitions,
                                                 std::span<RHIBufferTransition>     bufferTransitions,
                                                 std::span<RHITextureTransition>    textureTransitions)
{
    if (EnsureRecording())
    {
        VulkanPipelineBarrier barrier;

        bool hasBarrier = false;

        for (RHIMemoryTransition const& memoryTransition : memoryTransitions)
        {
            barrier.AddMemoryBarrier(memoryTransition.srcAccess, memoryTransition.dstAccess);

            hasBarrier = true;
        }

        for (RHIBufferTransition const& bufferTransition : bufferTransitions)
        {
            VulkanBuffer* pVulkanBuffer = TO_VK_BUFFER(bufferTransition.pBuffer);

            VkAccessFlags srcAccess =
                RHIBufferUsageToAccessFlagBits(bufferTransition.oldUsage, bufferTransition.oldAccessMode, srcStages);

            VkAccessFlags dstAccess =
                RHIBufferUsageToAccessFlagBits(bufferTransition.newUsage, bufferTransition.newAccessMode, dstStages);

            srcAccess |= bufferTransition.additionalSrcAccess;

            barrier.AddBufferBarrier(pVulkanBuffer->GetVkBuffer(), bufferTransition.offset, bufferTransition.size, srcAccess,
                                     dstAccess);

            hasBarrier = true;
        }

        for (RHITextureTransition const& textureTransition : textureTransitions)
        {
            VulkanTexture* pVulkanTexture = TO_VK_TEXTURE(textureTransition.pTexture);

            VkAccessFlags srcAccess       = textureTransition.GetSourceAccess();

            VkAccessFlags dstAccess =
                RHITextureUsageToAccessFlagBits(textureTransition.newUsage, textureTransition.newAccessMode);

            VkImageLayout oldLayout = ToVkImageLayout(RHITextureUsageToLayout(textureTransition.oldUsage));

            VkImageLayout newLayout = ToVkImageLayout(RHITextureUsageToLayout(textureTransition.newUsage));

            VkImageSubresourceRange subresourceRange{};

            ToVkImageSubresourceRange(textureTransition.subResourceRange, &subresourceRange);

            barrier.AddImageBarrier(pVulkanTexture->GetVkImage(), oldLayout, newLayout, subresourceRange, srcAccess, dstAccess);

            hasBarrier = true;
        }

        if (hasBarrier)
        {
            barrier.Execute(GetCommandBuffer()->GetVkHandle(), srcStages, dstStages);
        }
    }
}

void VulkanCommandListContext::RHIClearBuffer(RHIBuffer* pBuffer, uint64_t offset, uint64_t size)
{
    if (EnsureRecording())
    {
        vkCmdFillBuffer(GetCommandBuffer()->GetVkHandle(), TO_VK_BUFFER(pBuffer)->GetVkBuffer(), offset, size, 0);
    }
}

void VulkanCommandListContext::RHICopyBuffer(RHIBuffer* pSrcBuffer, RHIBuffer* pDstBuffer, const RHIBufferCopyRegion& region)
{
    if (EnsureRecording())
    {
        VkBufferCopy bufferCopy;

        bufferCopy.srcOffset = region.srcOffset;

        bufferCopy.dstOffset = region.dstOffset;

        bufferCopy.size      = region.size;

        vkCmdCopyBuffer(GetCommandBuffer()->GetVkHandle(), TO_VK_BUFFER(pSrcBuffer)->GetVkBuffer(),
                        TO_VK_BUFFER(pDstBuffer)->GetVkBuffer(), 1, &bufferCopy);
        MakeHostReadAvailable(pDstBuffer, region.dstOffset, region.size);
    }
}

void VulkanCommandListContext::MakeHostReadAvailable(RHIBuffer* buffer, uint64_t offset, uint64_t size)
{
    const VulkanBuffer* native = TO_VK_BUFFER(buffer);
    if (native->IsHostReadback())
    {
        // Completion orders the host read after this copy; host coherence only removes
        // the invalidate requirement. Make the device writes available to the host domain.
        VkBufferMemoryBarrier barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
        barrier.srcAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask       = VK_ACCESS_HOST_READ_BIT;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.buffer              = native->GetVkBuffer();
        barrier.offset              = offset;
        barrier.size                = size;
        vkCmdPipelineBarrier(GetCommandBuffer()->GetVkHandle(), VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0,
                             0, nullptr, 1, &barrier, 0, nullptr);
    }
}

void VulkanCommandListContext::RHIClearTexture(RHITexture*                       pTexture,
                                               const Color&                      color,
                                               const RHITextureSubresourceRange& range)
{
    if (pTexture == nullptr || FormatIsDepthStencil(pTexture->GetBaseInfo().format)
        || FormatIsDepthOnly(pTexture->GetBaseInfo().format) || FormatIsStencilOnly(pTexture->GetBaseInfo().format))
    {
        LatchError(MakeRHIError(RHIErrorCode::eInvalidArgument, "ClearTexture requires a color format", __FILE__, __LINE__));
    }
    if (EnsureRecording())
    {
        VkImageSubresourceRange vkRange;

        ToVkImageSubresourceRange(range, &vkRange);

        const VkClearColorValue colorValue = ToVkClearColor(color, pTexture->GetBaseInfo().format);

        vkCmdClearColorImage(GetCommandBuffer()->GetVkHandle(), TO_VK_TEXTURE(pTexture)->GetVkImage(),
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &colorValue, 1, &vkRange);
    }
}

void VulkanCommandListContext::RHICopyTexture(RHITexture*                     pSrcTexture,
                                              RHITexture*                     pDstTexture,
                                              std::span<RHITextureCopyRegion> regions)
{
    if (EnsureRecording())
    {
        SmallVector<VkImageCopy, 4> copies(regions.size());

        for (uint32_t i = 0; i < regions.size(); i++)
        {
            ToVkImageCopy(regions[i], &copies[i]);
        }

        vkCmdCopyImage(GetCommandBuffer()->GetVkHandle(), TO_VK_TEXTURE(pSrcTexture)->GetVkImage(),
                       VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, TO_VK_TEXTURE(pDstTexture)->GetVkImage(),
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, copies.size(), copies.data());
    }
}

void VulkanCommandListContext::RHIBlitTexture(RHITexture*                     pSrcTexture,
                                              RHITexture*                     pDstTexture,
                                              std::span<RHITextureBlitRegion> regions,
                                              RHISamplerFilter                filter)
{
    if (EnsureRecording())
    {
        SmallVector<VkImageBlit, 4> blits(regions.size());

        for (uint32_t i = 0; i < regions.size(); i++)
        {
            ToVkImageBlit(regions[i], &blits[i]);
        }

        vkCmdBlitImage(GetCommandBuffer()->GetVkHandle(), TO_VK_TEXTURE(pSrcTexture)->GetVkImage(),
                       VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, TO_VK_TEXTURE(pDstTexture)->GetVkImage(),
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, blits.size(), blits.data(), ToVkFilter(filter));
    }
}

void VulkanCommandListContext::RHICopyTextureToBuffer(RHITexture*                           pSrcTex,
                                                      RHIBuffer*                            pDstBuffer,
                                                      std::span<RHIBufferTextureCopyRegion> regions)
{
    if (EnsureRecording())
    {
        SmallVector<VkBufferImageCopy, 4> copies(regions.size());

        for (uint32_t i = 0; i < regions.size(); i++)
        {
            ToVkBufferImageCopy(regions[i], &copies[i]);
        }

        vkCmdCopyImageToBuffer(GetCommandBuffer()->GetVkHandle(), TO_VK_TEXTURE(pSrcTex)->GetVkImage(),
                               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, TO_VK_BUFFER(pDstBuffer)->GetVkBuffer(), copies.size(),
                               copies.data());
        MakeHostReadAvailable(pDstBuffer, 0, VK_WHOLE_SIZE);
    }
}

void VulkanCommandListContext::RHICopyBufferToTexture(RHIBuffer*                            pSrcBuffer,
                                                      RHITexture*                           pDstTexture,
                                                      std::span<RHIBufferTextureCopyRegion> regions)
{
    if (EnsureRecording())
    {
        SmallVector<VkBufferImageCopy, 4> copies(regions.size());

        for (uint32_t i = 0; i < copies.size(); i++)
        {
            ToVkBufferImageCopy(regions[i], &copies[i]);
        }

        vkCmdCopyBufferToImage(GetCommandBuffer()->GetVkHandle(), TO_VK_BUFFER(pSrcBuffer)->GetVkBuffer(),
                               TO_VK_TEXTURE(pDstTexture)->GetVkImage(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, copies.size(),
                               copies.data());
    }
}

void VulkanCommandListContext::RHIResolveTexture(RHITexture* pSrcTexture,
                                                 RHITexture* pDstTexture,
                                                 uint32_t    srcLayer,
                                                 uint32_t    srcMipmap,
                                                 uint32_t    dstLayer,
                                                 uint32_t    dstMipmap)
{
    if (EnsureRecording())
    {
        VkImageResolve region{};

        region.srcSubresource.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;

        region.srcSubresource.mipLevel       = srcMipmap;

        region.srcSubresource.baseArrayLayer = srcLayer;

        region.srcSubresource.layerCount     = 1;

        region.dstSubresource.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;

        region.dstSubresource.mipLevel       = dstMipmap;

        region.dstSubresource.baseArrayLayer = dstLayer;

        region.dstSubresource.layerCount     = 1;

        region.extent.width                  = std::max(1u, pSrcTexture->GetBaseInfo().width >> srcMipmap);

        region.extent.height                 = std::max(1u, pSrcTexture->GetBaseInfo().height >> srcMipmap);

        region.extent.depth                  = std::max(1u, pSrcTexture->GetBaseInfo().depth >> srcMipmap);

        vkCmdResolveImage(GetCommandBuffer()->GetVkHandle(), TO_VK_TEXTURE(pSrcTexture)->GetVkImage(),
                          VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, TO_VK_TEXTURE(pDstTexture)->GetVkImage(),
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    }
}

RHISubmissionResult VulkanRHI::SubmitCommandLists(std::span<RHICommandList*> lists)
{
    m_runtime->thread.CheckOwnership();

    SetSubmissionError({});

    const RHIStatus translated = TranslateCommandLists(lists, m_translatedBatch);

    RHISubmissionResult result = RHISubmissionResult::eRejected;

    if (translated)
    {
        result = SubmitTranslated(m_translatedBatch);
    }
    else
    {
        SetSubmissionError(translated.error);

        // A blocked device is terminal whatever error blocked it; only an unblocked rejection may be retried.
        result = translated.error.code == RHIErrorCode::eDeviceLost || AreSubmissionsBlocked() ? RHISubmissionResult::eFatal
                                                                                               : RHISubmissionResult::eRejected;
    }

    return result;
}

RHIStatus VulkanRHI::TranslateCommandLists(std::span<RHICommandList*> lists, VulkanTranslatedBatch& batch)
{
    m_runtime->thread.CheckOwnership();

    RHIStatus status;

    const size_t firstWorkload = batch.workloads.size();

    const size_t firstList     = batch.lists.size();

    const uint64_t transaction = m_pBindlessHeap->BeginTransaction();

    if (m_submissionBlocked)
    {
        status.error = GetSubmissionFailureCause();

        BlockSubmissions(status.error);
    }

    for (RHICommandList* commands : lists)
    {
        if (commands != nullptr)
        {
            commands->SetSubmittedSerial(0);
        }

        // Shared contexts must acquire each list's waits immediately before that list,
        // rather than accumulating later waits in front of earlier recorded commands.
        if (status
            && (commands == nullptr
                || !PrepareSubmissionDependencies(commands->GetContext(), commands->GetSubmissionDependencies())))
        {
            status.error = GetSubmissionFailureCause();

            BlockSubmissions(status.error);
        }

        if (status)
        {
            VulkanCommandListContext* context = static_cast<VulkanCommandListContext*>(commands->GetContext());

            context->SetRecordingTransaction(transaction);

            // Each recorded list owns an independent parameter snapshot. Raw contexts
            // may keep bindings between calls, but pooled lists must not inherit them.
            context->ResetShaderBindings();

            RHIWorkerTimer timer(RHIWorkerTime::eTranslation);
            commands->Execute();

            const uint32_t first = static_cast<uint32_t>(batch.workloads.size());

            uint64_t owner       = 0;
            context->CollectWorkloads(batch.workloads, &owner);

            status.error = context->GetRecordingError();

            if (status && commands->HasSubmissionBoundary())
            {
                // An empty group can relay an earlier same-queue producer to another
                // queue. Keep its signal without allocating an empty command buffer.
                if (batch.workloads.size() == first)
                {
                    batch.workloads.push_back(context->GetQueue()->AcquireWorkload());
                }

                batch.workloads.back()->m_submissionBoundary = true;
            }

            batch.transactions.push_back(owner);

            batch.lists.push_back({context, commands, first, static_cast<uint32_t>(batch.workloads.size()) - first});
        }
    }

    if (!status)
    {
        for (size_t i = firstWorkload; i < batch.workloads.size(); ++i)
        {
            batch.workloads[i]->m_pQueue->DiscardWorkload(batch.workloads[i]);
        }

        for (size_t i = firstList; i < batch.transactions.size(); ++i)
        {
            m_pBindlessHeap->ResolveTransaction(batch.transactions[i], false);
        }

        batch.workloads.resize(firstWorkload);

        batch.lists.resize(firstList);

        batch.transactions.resize(firstList);

        for (RHICommandList* commands : lists)
        {
            if (commands != nullptr && commands->GetContext() != nullptr)
            {
                commands->GetContext()->RHIDiscardRecording();
            }
        }
    }

    return status;
}

RHISubmissionResult VulkanRHI::SubmitTranslated(VulkanTranslatedBatch& batch)
{
    m_runtime->thread.CheckOwnership();

    for (VulkanWorkload* workload : batch.workloads)
    {
        workload->m_pQueue->EnqueueWorkload(workload);
    }

    HeapVector<VulkanQueue*> queues;

    for (uint32_t i = 0; i < ToUnderlying(RHICommandContextType::eMax); ++i)
    {
        VulkanQueue* queue = m_pDevice->GetQueue(static_cast<RHICommandContextType>(i));

        if (std::find(queues.begin(), queues.end(), queue) == queues.end())
        {
            queues.push_back(queue);
        }
    }

    RHISubmissionResult result = m_submissionBlocked ? RHISubmissionResult::eFatal : RHISubmissionResult::eSuccess;

    bool submitted             = false;

    for (VulkanQueue* queue : queues)
    {
        if (result != RHISubmissionResult::eSuccess)
        {
            break;
        }

        uint64_t serial  = 0;

        result           = queue->SubmitPendingWorkloads(serial);

        submitted       |= serial != 0;

        if (result == RHISubmissionResult::eRejected && submitted)
        {
            result = RHISubmissionResult::eFatal;
        }
    }

    if (result == RHISubmissionResult::eFatal)
    {
        BlockSubmissions(GetSubmissionFailureCause());
    }

    // Workload objects stay alive until every list has read the actual accepted serials.
    // Shared physical queues must not be polled again before these references are consumed.
    for (size_t i = 0; i < batch.lists.size(); ++i)
    {
        const VulkanTranslatedBatch::ListRange& range = batch.lists[i];

        uint64_t serial                               = 0;

        for (uint32_t j = 0; j < range.workloadCount; ++j)
        {
            serial = std::max(serial, batch.workloads[range.firstWorkload + j]->m_submissionSerial);
        }

        range.pCommands->SetSubmittedSerial(serial);

        m_pBindlessHeap->ResolveTransaction(batch.transactions[i],
                                            result == RHISubmissionResult::eSuccess || result == RHISubmissionResult::eFatal);

        if (result == RHISubmissionResult::eRejected)
        {
            range.pContext->RHIDiscardRecording();
        }
    }

    batch.workloads.clear();

    batch.lists.clear();

    batch.transactions.clear();

    for (VulkanQueue* queue : queues)
    {
        queue->DiscardPendingWorkloads(result == RHISubmissionResult::eFatal);
    }

    return result;
}

} // namespace zen
