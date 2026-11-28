#include "ZenRHI/RHIDiagnostics.h"
#include <algorithm>
#include "Vulkan/VulkanSynchronization.h"
#include "Vulkan/VulkanRHI.h"
#include "Vulkan/VulkanDevice.h"

namespace zen
{
VulkanSemaphore::VulkanSemaphore(VulkanDevice* pDevice, VkSemaphoreType semaphoreType, uint64_t initialValue) :
    m_pDevice(pDevice), m_type(semaphoreType)
{
    VkSemaphoreCreateInfo semaphoreCI;

    InitVkStruct(semaphoreCI, VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO);

    VkSemaphoreTypeCreateInfo semaphoreTypeCI;

    if (semaphoreType != VK_SEMAPHORE_TYPE_BINARY)
    {
        InitVkStruct(semaphoreTypeCI, VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO);

        semaphoreTypeCI.semaphoreType = semaphoreType;

        semaphoreTypeCI.initialValue  = initialValue;

        semaphoreCI.pNext             = &semaphoreTypeCI;
    }

    const VkResult result = vkCreateSemaphore(m_pDevice->GetVkHandle(), &semaphoreCI, nullptr, &m_semaphore);

    pDevice->GetRHI()->CheckDeviceLoss(result, "vkCreateSemaphore");

    m_error = MakeVulkanError(result, "vkCreateSemaphore", __FILE__, __LINE__);

    if (m_error.IsFailure())
    {
        m_semaphore = VK_NULL_HANDLE;

        ZEN_RHI_LOG_ERROR("vkCreateSemaphore failed: {}", int32_t(result));
    }
}

void VulkanSemaphore::SetDebugName(NameID name)
{
    m_pDevice->SetObjectName(VK_OBJECT_TYPE_SEMAPHORE, reinterpret_cast<uint64_t>(m_semaphore), name);
}

VkResult VulkanSemaphore::GetCounterValue(uint64_t& value) const
{
    ZEN_RHI_VERIFY(IsTimeline());

    value                 = 0;

    const VkResult result = vkGetSemaphoreCounterValue(m_pDevice->GetVkHandle(), m_semaphore, &value);

    if (result != VK_SUCCESS)
    {
        value = 0;
    }

    return result;
}

VkResult VulkanSemaphore::Wait(uint64_t value, uint64_t timeNS) const
{
    ZEN_RHI_VERIFY(IsTimeline());

    VkSemaphoreWaitInfo waitInfo;

    InitVkStruct(waitInfo, VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO);

    waitInfo.semaphoreCount = 1;

    waitInfo.pSemaphores    = &m_semaphore;

    waitInfo.pValues        = &value;

    return TimedRHICall(RHIWorkerTime::eWait, vkWaitSemaphores, m_pDevice->GetVkHandle(), &waitInfo, timeNS);
}

VulkanSemaphore::~VulkanSemaphore()
{
    vkDestroySemaphore(m_pDevice->GetVkHandle(), m_semaphore, nullptr);

    m_semaphore = VK_NULL_HANDLE;
}

void VulkanSemaphoreManager::Destroy()
{
#if defined(ZEN_DEBUG)
    ZEN_RHI_VERIFY(m_allocatedSemaphoreCount == m_usedSemaphores.size());
#endif

    for (VulkanSemaphore* pSem : m_usedSemaphores)
    {
        ZEN_DELETE(pSem);
    }

    m_usedSemaphores.clear();
#if defined(ZEN_DEBUG)
    m_allocatedSemaphoreCount = 0;
#endif
}

VulkanSemaphore* VulkanSemaphoreManager::CreateBinarySemaphore()
{
    VulkanSemaphore* result = ZEN_NEW() VulkanSemaphore(m_pDevice);

    m_error                 = result->GetError();

    if (!m_error.IsFailure())
    {
        m_usedSemaphores.push_back(result);
#if defined(ZEN_DEBUG)
        m_allocatedSemaphoreCount++;
#endif
    }
    else
    {
        ZEN_DELETE(result);

        result = nullptr;
    }

    return result;
}

void VulkanSemaphoreManager::DestroySemaphore(VulkanSemaphore*& sem)
{
    if (sem != nullptr)
    {
        zen::VulkanSemaphore** it = std::find(m_usedSemaphores.begin(), m_usedSemaphores.end(), sem);

        if (it == m_usedSemaphores.end())
        {
            ZEN_RHI_VERIFY_MSG_F(false, "Cannot destroy a semaphore not owned by this manager");
        }

        m_usedSemaphores.erase(it);

        ZEN_DELETE(sem);

        sem = nullptr;
#if defined(ZEN_DEBUG)
        --m_allocatedSemaphoreCount;
#endif
    }
}

void VulkanPipelineBarrier::AddImageBarrier(VkImage                        image,
                                            VkImageLayout                  srcLayout,
                                            VkImageLayout                  dstLayout,
                                            const VkImageSubresourceRange& range,
                                            VkAccessFlags                  srcAccess,
                                            VkAccessFlags                  dstAccess)
{
    VkImageMemoryBarrier barrier;

    InitVkStruct(barrier, VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER);

    barrier.image               = image;

    barrier.srcAccessMask       = srcAccess;

    barrier.dstAccessMask       = dstAccess;

    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;

    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;

    barrier.oldLayout           = srcLayout;

    barrier.newLayout           = dstLayout;

    barrier.subresourceRange    = range;

    m_imageBarriers.emplace_back(barrier);
}

void VulkanPipelineBarrier::AddBufferBarrier(VkBuffer      buffer,
                                             uint64_t      offset,
                                             uint64_t      size,
                                             VkAccessFlags srcAccess,
                                             VkAccessFlags dstAccess)
{
    VkBufferMemoryBarrier bufferBarrier;

    InitVkStruct(bufferBarrier, VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER);

    bufferBarrier.buffer              = buffer;

    bufferBarrier.offset              = offset;

    bufferBarrier.size                = size;

    bufferBarrier.srcAccessMask       = srcAccess;

    bufferBarrier.dstAccessMask       = dstAccess;

    bufferBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;

    bufferBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;

    m_bufferBarriers.emplace_back(bufferBarrier);
}

void VulkanPipelineBarrier::AddMemoryBarrier(VkAccessFlags srcAccess, VkAccessFlags dstAccess)
{
    VkMemoryBarrier memoryBarrier;

    InitVkStruct(memoryBarrier, VK_STRUCTURE_TYPE_MEMORY_BARRIER);

    memoryBarrier.srcAccessMask = srcAccess;

    memoryBarrier.dstAccessMask = dstAccess;

    m_memoryBarriers.emplace_back(memoryBarrier);
}

void VulkanPipelineBarrier::Execute(VkCommandBuffer      cmdBuffer,
                                    VkPipelineStageFlags srcStageFlags,
                                    VkPipelineStageFlags dstStageFlags)
{
    if (!m_memoryBarriers.empty() || !m_bufferBarriers.empty() || !m_imageBarriers.empty())
    {
        vkCmdPipelineBarrier(cmdBuffer, srcStageFlags, dstStageFlags, 0, m_memoryBarriers.size(), m_memoryBarriers.data(),
                             m_bufferBarriers.size(), m_bufferBarriers.data(), m_imageBarriers.size(), m_imageBarriers.data());

        m_memoryBarriers.clear();

        m_bufferBarriers.clear();

        m_imageBarriers.clear();
    }
}

} // namespace zen
