#include "ZenRHI/RHIDiagnostics.h"
#include <algorithm>
#include "Vulkan/VulkanQueue.h"
#include "Vulkan/VulkanDevice.h"
#include "Vulkan/VulkanSynchronization.h"
#include "Vulkan/VulkanCommandList.h"
#include "Vulkan/VulkanRHI.h"
#include "Vulkan/VulkanBindlessHeap.h"
#include "Vulkan/VulkanBuffer.h"
#include <chrono>

namespace zen
{
namespace
{
void AppendCommandBufferPool(HeapVector<VulkanCommandBufferPool*>& pools, VulkanCommandBufferPool* pPool)
{
    if (pPool != nullptr && std::find(pools.begin(), pools.end(), pPool) == pools.end())
    {
        pools.push_back(pPool);
    }
}
} // namespace

VulkanQueue::VulkanQueue(VulkanDevice* pDevice, uint32_t familyIndex, uint32_t queueIndex) :
    m_pDevice(pDevice), m_familyIndex(familyIndex), m_queueIndex(queueIndex)
{
    vkGetDeviceQueue(m_pDevice->GetVkHandle(), m_familyIndex, m_queueIndex, &m_handle);

    m_pTimelineSemaphore = ZEN_NEW() VulkanSemaphore(m_pDevice, VK_SEMAPHORE_TYPE_TIMELINE, 0);
}

RHIError VulkanQueue::GetCreationError() const
{
    RHIError error;
    if (!m_handle)
    {
        error = MakeRHIError(RHIErrorCode::eBackendFailure, "vkGetDeviceQueue", __FILE__, __LINE__);
    }
    else
    {
        error = m_pTimelineSemaphore
                  ? m_pTimelineSemaphore->GetError()
                  : MakeRHIError(RHIErrorCode::eOutOfHostMemory, "Allocate queue timeline semaphore", __FILE__, __LINE__);
    }
    return error;
}

VulkanQueue::~VulkanQueue()
{
    VerifyTeardownOwnership(m_acquiredCommandBufferPools == 0,
                            "All RHI command contexts must be released before queue teardown", __FILE__, __LINE__);

    while (!m_workloadsPendingSubmit.Empty())
    {
        VulkanWorkload* pWorkload = m_workloadsPendingSubmit.Peek();

        m_workloadsPendingSubmit.Pop();

        DestroyWorkload(pWorkload);
    }

    while (!m_workloadsPendingProcess.Empty())
    {
        VulkanWorkload* pWorkload = m_workloadsPendingProcess.Peek();

        m_workloadsPendingProcess.Pop();

        DestroyWorkload(pWorkload);
    }

    for (VulkanWorkload* pWorkload : m_workloadPool)
    {
        DestroyWorkload(pWorkload);
    }

    for (VulkanWorkload* workload : m_abandonedWorkloads)
    {
        DestroyWorkload(workload);
    }

    m_pDevice->GetRHI()->GetLifetimeTracker().RemoveQueue(this);

    for (VulkanCommandBufferPool* pCmdBufferPool : m_cmdBufferPools)
    {
        ZEN_DELETE(pCmdBufferPool);
    }

    if (m_pTimelineSemaphore != nullptr)
    {
        ZEN_DELETE(m_pTimelineSemaphore);

        m_pTimelineSemaphore = nullptr;
    }
}

VulkanCommandBufferPool* VulkanQueue::AcquireCommandBufferPool()
{
    VulkanCommandBufferPool* pResult = nullptr;

    if (m_cmdBufferPools.empty())
    {
        pResult = ZEN_NEW() VulkanCommandBufferPool(this);
    }
    else
    {
        pResult = m_cmdBufferPools.back();
        m_cmdBufferPools.pop_back();
    }

    ++m_acquiredCommandBufferPools;

    return pResult;
}

void VulkanQueue::RecycleCommandBufferPool(VulkanCommandBufferPool* pCmdBufferPool)
{
    ZEN_RHI_VERIFY(pCmdBufferPool->GetQueue() == this);

    ZEN_RHI_VERIFY(m_acquiredCommandBufferPools != 0);

    --m_acquiredCommandBufferPools;

    pCmdBufferPool->FreeUnusedCommandBuffers();

    if (pCmdBufferPool->GetVkHandle() != VK_NULL_HANDLE)
    {
        m_cmdBufferPools.emplace_back(pCmdBufferPool);
    }
    else
    {
        ZEN_DELETE(pCmdBufferPool);
    }
}

VulkanWorkload* VulkanQueue::AcquireWorkload()
{
    VulkanWorkload* result{};

    if (!m_workloadPool.empty())
    {
        VulkanWorkload* pWorkload = m_workloadPool.back();

        m_workloadPool.pop_back();

        result = pWorkload;
    }
    else
    {
        result = ZEN_NEW() VulkanWorkload(this);
    }

    return result;
}

void VulkanQueue::DiscardWorkload(VulkanWorkload* pWorkload)
{
    for (VulkanCommandBuffer* buffer : pWorkload->m_commandBuffers)
    {
        buffer->Discard();
    }

    ReleaseWorkload(pWorkload);

    m_pDevice->GetRHI()->GetLifetimeTracker().Collect();
}

void VulkanQueue::ReleaseWorkload(VulkanWorkload* pWorkload)
{
    if (pWorkload != nullptr)
    {
        ZEN_RHI_VERIFY(pWorkload->m_pQueue == this);

        pWorkload->m_commandBuffers.clear();

        pWorkload->m_ownership.Release(*m_pDevice->GetRHI());

        pWorkload->m_submissionSerial   = 0;

        pWorkload->m_submissionBoundary = false;

        pWorkload->m_pMergedInto        = nullptr;

        pWorkload->m_waitSemaphoreInfos.clear();

        pWorkload->m_signalSemaphoreInfos.clear();

        for (VulkanWorkload* merged : pWorkload->m_mergedWorkloads)
        {
            ReleaseWorkload(merged);
        }

        pWorkload->m_mergedWorkloads.clear();

        m_workloadPool.push_back(pWorkload);
    }
}

void VulkanQueue::DestroyWorkload(VulkanWorkload* pWorkload)
{
    if (pWorkload != nullptr)
    {
        pWorkload->m_ownership.Release(*m_pDevice->GetRHI());

        for (VulkanWorkload* pMerged : pWorkload->m_mergedWorkloads)
        {
            DestroyWorkload(pMerged);
        }

        pWorkload->m_mergedWorkloads.clear();

        ZEN_DELETE(pWorkload);
    }
}

bool VulkanQueue::CanMergeWorkloads(const VulkanWorkload* pPreviousWorkload, const VulkanWorkload* pCurrentWorkload)
{
    // Preserve a group's earlier completion signal even when a later group has no waits.
    return !pPreviousWorkload->m_submissionBoundary && pPreviousWorkload->m_signalSemaphoreInfos.empty()
        && pCurrentWorkload->m_waitSemaphoreInfos.empty();
}

static RHISubmissionResult SubmissionFailure(VulkanRHI& rhi, VkResult result)
{
    rhi.CheckDeviceLoss(result, "vkQueueSubmit");

    rhi.SetSubmissionError(MakeVulkanError(result, "vkQueueSubmit", __FILE__, __LINE__));

    ZEN_RHI_LOG_ERROR("Vulkan queue submission failed: {}", int32_t(result));

    // Vulkan guarantees unchanged resource/semaphore state for these errors only.
    const bool rejected = result == VK_ERROR_OUT_OF_HOST_MEMORY || result == VK_ERROR_OUT_OF_DEVICE_MEMORY;

    return rejected ? RHISubmissionResult::eRejected : RHISubmissionResult::eFatal;
}

void VulkanQueue::DiscardPendingWorkloads(bool uncertain)
{
    while (!m_workloadsPendingSubmit.Empty())
    {
        VulkanWorkload* workload = m_workloadsPendingSubmit.Peek();

        m_workloadsPendingSubmit.Pop();

        if (uncertain)
        {
            m_abandonedWorkloads.push_back(workload);
        }
        else
        {
            for (VulkanCommandBuffer* buffer : workload->m_commandBuffers)
            {
                buffer->Discard();
            }

            ReleaseWorkload(workload);
        }
    }

    m_pDevice->GetRHI()->GetLifetimeTracker().Collect();
}

void VulkanQueue::MergeWorkloads(const HeapVector<VulkanWorkload*>& workloadsToSubmit, WorkloadMergeResult& outMergeResult)
{
    outMergeResult.workloadsToSubmit.reserve(workloadsToSubmit.size());

    for (VulkanWorkload* pWorkload : workloadsToSubmit)
    {
        if (!outMergeResult.workloadsToSubmit.empty() && CanMergeWorkloads(outMergeResult.workloadsToSubmit.back(), pWorkload))
        {
            outMergeResult.workloadsToSubmit.back()->Merge(pWorkload);

            continue;
        }

        outMergeResult.workloadsToSubmit.push_back(pWorkload);
    }

    uint64_t nextSerial = m_lastSubmittedSerial;

    for (VulkanWorkload* pMergedWorkload : outMergeResult.workloadsToSubmit)
    {
        const uint64_t submissionSerial           = ++nextSerial;

        pMergedWorkload->m_submissionSerial       = submissionSerial;

        outMergeResult.totalWaitSemaphoreCount   += pMergedWorkload->m_waitSemaphoreInfos.size();

        outMergeResult.totalSignalSemaphoreCount += pMergedWorkload->m_signalSemaphoreInfos.size() + 1;

        outMergeResult.totalCommandBufferCount   += pMergedWorkload->m_commandBuffers.size();
    }

    for (VulkanWorkload* pWorkload : workloadsToSubmit)
    {
        if (pWorkload->m_pMergedInto != nullptr)
        {
            pWorkload->m_submissionSerial = pWorkload->m_pMergedInto->m_submissionSerial;
        }
    }
}

void VulkanQueue::BuildTimelineSubmitBatch(const WorkloadMergeResult& mergeResult, TimelineSubmitBatch& outSubmitBatch)
{
    outSubmitBatch.submitInfos.reserve(mergeResult.workloadsToSubmit.size());

    outSubmitBatch.timelineSubmitInfos.reserve(mergeResult.workloadsToSubmit.size());

    outSubmitBatch.commandBuffers.reserve(mergeResult.totalCommandBufferCount);

    outSubmitBatch.waitSemaphores.reserve(mergeResult.totalWaitSemaphoreCount);

    outSubmitBatch.signalSemaphores.reserve(mergeResult.totalSignalSemaphoreCount);

    outSubmitBatch.waitStageMasks.reserve(mergeResult.totalWaitSemaphoreCount);

    outSubmitBatch.waitSemaphoreValues.reserve(mergeResult.totalWaitSemaphoreCount);

    outSubmitBatch.signalSemaphoreValues.reserve(mergeResult.totalSignalSemaphoreCount);

    for (VulkanWorkload* pWorkload : mergeResult.workloadsToSubmit)
    {
        AppendTimelineSubmitWorkload(pWorkload, outSubmitBatch);
    }
}

void VulkanQueue::AppendTimelineSubmitWorkload(VulkanWorkload* pWorkload, TimelineSubmitBatch& outSubmitBatch)
{
    const size_t firstWaitSemaphoreIndex   = outSubmitBatch.waitSemaphores.size();

    const size_t firstWaitValueIndex       = outSubmitBatch.waitSemaphoreValues.size();

    const size_t firstSignalSemaphoreIndex = outSubmitBatch.signalSemaphores.size();

    const size_t firstSignalValueIndex     = outSubmitBatch.signalSemaphoreValues.size();

    const size_t firstCommandBufferIndex   = outSubmitBatch.commandBuffers.size();

    for (VulkanWorkload::WaitSemaphoreInfo const& waitInfo : pWorkload->m_waitSemaphoreInfos)
    {
        outSubmitBatch.waitSemaphores.push_back(waitInfo.pSemaphore->GetVkHandle());

        outSubmitBatch.waitStageMasks.push_back(waitInfo.waitFlags);

        outSubmitBatch.waitSemaphoreValues.push_back(waitInfo.value);
    }

    for (VulkanCommandBuffer* pCmdBuffer : pWorkload->m_commandBuffers)
    {
        outSubmitBatch.commandBuffers.push_back(pCmdBuffer->GetVkHandle());
    }

    for (VulkanWorkload::SignalSemaphoreInfo const& signalInfo : pWorkload->m_signalSemaphoreInfos)
    {
        outSubmitBatch.signalSemaphores.push_back(signalInfo.pSemaphore->GetVkHandle());

        outSubmitBatch.signalSemaphoreValues.push_back(signalInfo.value);
    }

    outSubmitBatch.signalSemaphores.push_back(m_pTimelineSemaphore->GetVkHandle());

    outSubmitBatch.signalSemaphoreValues.push_back(pWorkload->m_submissionSerial);

    VkTimelineSemaphoreSubmitInfo timelineSubmitInfo;

    InitVkStruct(timelineSubmitInfo, VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO);

    timelineSubmitInfo.waitSemaphoreValueCount =
        static_cast<uint32_t>(outSubmitBatch.waitSemaphoreValues.size() - firstWaitValueIndex);

    timelineSubmitInfo.pWaitSemaphoreValues = timelineSubmitInfo.waitSemaphoreValueCount > 0
                                                ? outSubmitBatch.waitSemaphoreValues.data() + firstWaitValueIndex
                                                : nullptr;

    timelineSubmitInfo.signalSemaphoreValueCount =
        static_cast<uint32_t>(outSubmitBatch.signalSemaphoreValues.size() - firstSignalValueIndex);

    timelineSubmitInfo.pSignalSemaphoreValues = timelineSubmitInfo.signalSemaphoreValueCount > 0
                                                  ? outSubmitBatch.signalSemaphoreValues.data() + firstSignalValueIndex
                                                  : nullptr;

    outSubmitBatch.timelineSubmitInfos.emplace_back(timelineSubmitInfo);

    VkSubmitInfo submitInfo;

    InitVkStruct(submitInfo, VK_STRUCTURE_TYPE_SUBMIT_INFO);

    submitInfo.pNext              = &outSubmitBatch.timelineSubmitInfos[outSubmitBatch.submitInfos.size()];

    submitInfo.waitSemaphoreCount = static_cast<uint32_t>(outSubmitBatch.waitSemaphores.size() - firstWaitSemaphoreIndex);

    submitInfo.pWaitSemaphores =
        submitInfo.waitSemaphoreCount > 0 ? outSubmitBatch.waitSemaphores.data() + firstWaitSemaphoreIndex : nullptr;

    submitInfo.pWaitDstStageMask =
        submitInfo.waitSemaphoreCount > 0 ? outSubmitBatch.waitStageMasks.data() + firstWaitSemaphoreIndex : nullptr;

    submitInfo.commandBufferCount = static_cast<uint32_t>(outSubmitBatch.commandBuffers.size() - firstCommandBufferIndex);

    submitInfo.pCommandBuffers =
        submitInfo.commandBufferCount > 0 ? outSubmitBatch.commandBuffers.data() + firstCommandBufferIndex : nullptr;

    submitInfo.signalSemaphoreCount = static_cast<uint32_t>(outSubmitBatch.signalSemaphores.size() - firstSignalSemaphoreIndex);

    submitInfo.pSignalSemaphores =
        submitInfo.signalSemaphoreCount > 0 ? outSubmitBatch.signalSemaphores.data() + firstSignalSemaphoreIndex : nullptr;

    outSubmitBatch.submitInfos.emplace_back(submitInfo);
}

void VulkanQueue::PrepareBindlessWrites(std::span<VulkanWorkload*> workloads)
{
    VulkanBindlessHeap* manager = m_pDevice->GetRHI()->GetBindlessHeap();
    bool                staged  = false;
    for (const VulkanWorkload* workload : workloads)
    {
        if (workload->m_ownership.bindlessTransaction != 0)
        {
            manager->PrepareTransaction(workload->m_ownership.bindlessTransaction);
            staged = true;
        }
    }
    if (staged)
    {
        manager->Flush();
    }
}

void VulkanQueue::AcceptWorkload(VulkanWorkload* workload, uint64_t submissionSerial)
{
    workload->m_ownership.Accept(*m_pDevice->GetRHI(), this, submissionSerial);

    for (VulkanWorkload* merged : workload->m_mergedWorkloads)
    {
        AcceptWorkload(merged, submissionSerial);
    }
}

void VulkanQueue::QueueSubmittedWorkload(VulkanWorkload* pWorkload, uint64_t submissionSerial)
{
    pWorkload->m_submissionSerial = submissionSerial;
    // One acceptance path for every tracked resource, including merged recordings.
    AcceptWorkload(pWorkload, submissionSerial);

    // Only called after vkQueueSubmit succeeds. Merging already transfers all
    // signal entries to the root, so no separate receipt or child traversal is needed.
    for (const VulkanWorkload::SignalSemaphoreInfo& signal : pWorkload->m_signalSemaphoreInfos)
    {
        signal.pSemaphore->m_pSignalQueue           = this;

        signal.pSemaphore->m_signalSubmissionSerial = submissionSerial;

        ++signal.pSemaphore->m_signalGeneration;
    }

    for (VulkanCommandBuffer* pCmdBuffer : pWorkload->m_commandBuffers)
    {
        pCmdBuffer->SetSubmitted();
    }

    m_workloadsPendingProcess.Push(pWorkload);
}

RHISubmissionResult VulkanQueue::SubmitWorkloads(uint64_t& lastSubmissionSerial)
{
    RHISubmissionResult returnValue{};

    HeapVector<VulkanWorkload*> workloadsToSubmit;

    workloadsToSubmit.reserve(m_workloadsPendingSubmit.Size());

    while (!m_workloadsPendingSubmit.Empty())
    {
        VulkanWorkload* pWorkload = m_workloadsPendingSubmit.Peek();

        m_workloadsPendingSubmit.Pop();

        workloadsToSubmit.push_back(pWorkload);
    }

    if (workloadsToSubmit.empty())
    {
        returnValue = RHISubmissionResult::eSuccess;
    }
    else
    {
        WorkloadMergeResult mergeResult;

        PrepareBindlessWrites(workloadsToSubmit);
        MergeWorkloads(workloadsToSubmit, mergeResult);

        TimelineSubmitBatch submitBatch;

        BuildTimelineSubmitBatch(mergeResult, submitBatch);

        m_pDevice->GetRHI()->GetExecutionCounterStorage().Increment(
            m_pDevice->GetRHI()->GetExecutionCounterStorage().submissions);

        const VkResult result =
            TimedRHICall(RHIWorkerTime::eSubmission, vkQueueSubmit, m_handle,
                         static_cast<uint32_t>(submitBatch.submitInfos.size()), submitBatch.submitInfos.data(), VK_NULL_HANDLE);

        if (result != VK_SUCCESS)
        {
            for (VulkanWorkload* workload : workloadsToSubmit)
            {
                workload->m_submissionSerial = 0;
            }

            // Keep roots (which own merged children) until platform/context references are consumed.
            for (VulkanWorkload* workload : mergeResult.workloadsToSubmit)
            {
                m_workloadsPendingSubmit.Push(workload);
            }

            returnValue = SubmissionFailure(*m_pDevice->GetRHI(), result);
        }
        else
        {
            lastSubmissionSerial  = workloadsToSubmit.back()->m_submissionSerial;

            m_lastSubmittedSerial = lastSubmissionSerial;

            // Roots own merged children; enqueue each owned tree once for completion/reclamation.
            for (VulkanWorkload* pWorkload : mergeResult.workloadsToSubmit)
            {
                QueueSubmittedWorkload(pWorkload, pWorkload->m_submissionSerial);
            }

            returnValue = RHISubmissionResult::eSuccess;
        }
    }

    return returnValue;
}

RHISubmissionResult VulkanQueue::SubmitPendingWorkloads(uint64_t& serial)
{
    RHISubmissionResult result{};

    serial = 0;

    // Retire previously completed submissions before appending new ones. This keeps the pending
    // queue bounded to in-flight GPU work instead of growing for the whole app lifetime.
    ProcessPendingWorkloads(0);

    if (m_pDevice->GetRHI()->AreSubmissionsBlocked())
    {
        result = RHISubmissionResult::eFatal;
    }
    else if (m_workloadsPendingSubmit.Empty())
    {
        result = RHISubmissionResult::eSuccess;
    }
    else
    {
        result = SubmitWorkloads(serial);
    }

    return result;
}

void VulkanQueue::ProcessPendingWorkloads(uint64_t timeToWaitNS, uint64_t maxSubmissionSerial)
{
    const std::chrono::steady_clock::time_point start = timeToWaitNS != 0 && timeToWaitNS != UINT64_MAX
                                                          ? std::chrono::steady_clock::now()
                                                          : std::chrono::steady_clock::time_point{};

    HeapVector<VulkanCommandBufferPool*> cmdBufferPoolsToTrim;

    uint64_t completedValue = 0;

    bool timelineQueried    = false;

    while (!m_pDevice->GetRHI()->AreSubmissionsBlocked())
    {
        if (m_workloadsPendingProcess.Empty())
        {
            break;
        }

        VulkanWorkload* pWorkload = m_workloadsPendingProcess.Peek();

        if (pWorkload->m_submissionSerial > maxSubmissionSerial)
        {
            break;
        }

        // A finite timeout covers the entire call.
        uint64_t remainingNS = timeToWaitNS;

        if (timeToWaitNS != UINT64_MAX && timeToWaitNS != 0)
        {
            const uint64_t elapsed = uint64_t(
                std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count());

            remainingNS = elapsed < timeToWaitNS ? timeToWaitNS - elapsed : 0;
        }

        VkResult result = VK_SUCCESS;

        if (!timelineQueried)
        {
            result          = m_pTimelineSemaphore->GetCounterValue(completedValue);

            timelineQueried = true;
        }

        if (result == VK_SUCCESS && completedValue < pWorkload->m_submissionSerial)
        {
            result = remainingNS == 0 ? VK_NOT_READY : m_pTimelineSemaphore->Wait(pWorkload->m_submissionSerial, remainingNS);

            if (result == VK_SUCCESS)
            {
                // The successful wait proves this serial completed without another query.
                completedValue = pWorkload->m_submissionSerial;
            }
        }

        if (result != VK_SUCCESS)
        {
            if (result != VK_NOT_READY && result != VK_TIMEOUT)
            {
                ZEN_RHI_LOG_ERROR("Vulkan queue {} completion query/wait failed: {}", m_familyIndex, int32_t(result));

                m_pDevice->GetRHI()->CheckDeviceLoss(result, "queue completion");

                m_pDevice->GetRHI()->BlockSubmissions(MakeVulkanError(result, "queue completion", __FILE__, __LINE__));
            }

            // Incomplete work and failures both retain ownership; only failures are terminal.
            break;
        }

        m_workloadsPendingProcess.Pop();

        for (VulkanCommandBuffer* pCmdBuffer : pWorkload->m_commandBuffers)
        {
            pCmdBuffer->SetCompleted();

            AppendCommandBufferPool(cmdBufferPoolsToTrim, pCmdBuffer->GetCommandBufferPool());
        }

        const uint64_t retired = pWorkload->m_submissionSerial;
        ReleaseWorkload(pWorkload);
        m_lastCompletedSerial.store(retired, std::memory_order_release);
    }

    // Trim once per pool after retiring all currently completed workloads.
    for (VulkanCommandBufferPool* pCmdBufferPool : cmdBufferPoolsToTrim)
    {
        pCmdBufferPool->FreeUnusedCommandBuffers();
    }

    m_pDevice->GetRHI()->GetLifetimeTracker().Collect();
}

bool VulkanQueue::WaitForCompletion(uint64_t submissionSerial, uint64_t timeToWaitNS)
{
    bool result{};

    if (submissionSerial > m_lastSubmittedSerial)
    {
        ZEN_RHI_LOG_ERROR("Vulkan queue {}: cannot wait for unsubmitted serial {} (last submitted {})", m_familyIndex,
                          submissionSerial, m_lastSubmittedSerial);

        result = false;
    }
    else if (submissionSerial == 0 || m_lastCompletedSerial >= submissionSerial)
    {
        result = true;
    }
    else
    {
        ProcessPendingWorkloads(timeToWaitNS, submissionSerial);

        result = m_lastCompletedSerial >= submissionSerial;
    }

    return result;
}
} // namespace zen
