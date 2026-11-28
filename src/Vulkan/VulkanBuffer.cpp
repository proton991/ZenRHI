#include "ZenRHI/RHIDiagnostics.h"
#include "Vulkan/VulkanResourceSharing.h"
#include "Vulkan/VulkanRHI.h"
#include "Vulkan/VulkanBuffer.h"
#include "Core/RHIValidation.h"
#include "Vulkan/VulkanCommon.h"
#include "Vulkan/VulkanDevice.h"
#include "Vulkan/VulkanQueue.h"
#include "Vulkan/VulkanResourceAllocator.h"
#include "Vulkan/VulkanTypes.h"

namespace zen
{
namespace
{
template <class Operation>
void WithBufferCreateInfo(VulkanRHI& rhi, uint64_t size, RHIFlags<RHIBufferUsageFlagBits> usage, Operation&& operation)
{
    VkBufferCreateInfo info;
    InitVkStruct(info, VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO);
    info.size           = static_cast<VkDeviceSize>(size);
    info.usage          = usage;
    const bool transfer = (info.usage & (VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT)) != 0;
    AllocateWithQueueSharing(info, rhi.GetDevice()->GetQueueSharing(transfer), [&info, &operation] { operation(info); });
}

uint64_t QueryBufferAllocationSize(VulkanRHI& rhi, const VkBufferCreateInfo& info)
{
    uint64_t       size   = 0;
    VkBuffer       buffer = VK_NULL_HANDLE;
    const VkDevice device = rhi.GetVkDevice();
    const VkResult result = vkCreateBuffer(device, &info, nullptr, &buffer);
    if (result == VK_SUCCESS)
    {
        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements(device, buffer, &requirements);
        size = requirements.size;
        vkDestroyBuffer(device, buffer, nullptr);
    }
    else
    {
        rhi.CheckDeviceLoss(result, "vkCreateBuffer allocation size query");
        ZEN_RHI_LOG_ERROR("Buffer allocation size query failed: {}", GetResultString(result));
    }
    return size;
}
} // namespace

RHIBuffer* VulkanRHI::CreateBuffer(const RHIBufferCreateInfo& createInfo)
{
    RHIWorkerTimer timer(RHIWorkerTime::eResource);
    return AreSubmissionsBlocked() ? nullptr : VulkanBuffer::CreateObject(this, createInfo).GetValueOr(nullptr);
}

uint64_t VulkanRHI::GetBufferAllocationSize(const RHIBufferCreateInfo& createInfo)
{
    uint64_t size = 0;
    if (!AreSubmissionsBlocked() && ValidateBufferCreateInfo(createInfo, QueryGPUInfo()) == nullptr)
    {
        WithBufferCreateInfo(*this, createInfo.size, createInfo.usageFlags,
                             [this, &size](const VkBufferCreateInfo& info) { size = QueryBufferAllocationSize(*this, info); });
    }
    return size;
}

RHIResult<VulkanBuffer*> VulkanBuffer::CreateObject(VulkanRHI* pRHI, const RHIBufferCreateInfo& createInfo)
{
    const char* rule = ValidateBufferCreateInfo(createInfo, pRHI->QueryGPUInfo());

    if (rule != nullptr)
    {
        ZEN_RHI_LOG_ERROR("Buffer description: {}", rule);
    }

    VulkanBuffer* buffer = rule == nullptr ? NewVulkanResource<VulkanBuffer>(pRHI, pRHI, createInfo) : nullptr;

    return buffer != nullptr ? KeepIfInitialized(buffer, buffer->Init())
                             : RejectDescription<VulkanBuffer>(rule, __FILE__, __LINE__);
}

RHIError VulkanBuffer::Init()
{
    // Replaced by the allocation result once the queue-sharing setup runs the allocation.
    RHIError error = MakeRHIError(RHIErrorCode::eBackendFailure, "Create buffer", __FILE__, __LINE__);

    WithBufferCreateInfo(*m_pRHI, m_requiredSize, m_usageFlags, [this, &error](const VkBufferCreateInfo& info) {
        error = m_pRHI->GetMemoryAllocator()->AllocBuffer(&info, m_allocateType, &m_vkBuffer, &m_memAlloc);
    });
    if (m_vkBuffer != VK_NULL_HANDLE && m_usageFlags.HasFlag(RHIBufferUsageFlagBits::eDeviceAddress))
    {
        VkBufferDeviceAddressInfo addressInfo{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
        addressInfo.buffer = m_vkBuffer;
        m_deviceAddress    = vkGetBufferDeviceAddress(m_pRHI->GetVkDevice(), &addressInfo);
    }

    if (m_vkBuffer != VK_NULL_HANDLE)
    {
        RHIExecutionCounterStorage& counters = m_pRHI->GetExecutionCounterStorage();

        counters.Increment(counters.buffersCreated);
    }

    return error;
}

void VulkanBuffer::Destroy()
{
    if (m_bufferView != VK_NULL_HANDLE)
    {
        vkDestroyBufferView(m_pRHI->GetVkDevice(), m_bufferView, nullptr);
    }

    if (m_vkBuffer != VK_NULL_HANDLE || m_memAlloc.handle != VK_NULL_HANDLE)
    {
        if (m_vkBuffer != VK_NULL_HANDLE)
        {
            RHIExecutionCounterStorage& counters = m_pRHI->GetExecutionCounterStorage();

            counters.Increment(counters.buffersDestroyed);
        }

        m_pRHI->GetMemoryAllocator()->FreeBuffer(m_vkBuffer, m_memAlloc);
    }

    DeleteVulkanResource(m_pRHI, this);
}

uint8_t* VulkanBuffer::Map()
{
    return static_cast<uint8_t*>(m_memAlloc.info.pMappedData);
}

void VulkanBuffer::Unmap()
{
    // CPU allocations stay mapped until VMA destroys the buffer.
}

bool VulkanBuffer::SetTexelFormat(DataFormat format)
{
    return m_pRHI->GetRuntime().thread.Invoke(&VulkanBuffer::SetTexelFormatOnRHIThread, this, format);
}

bool VulkanBuffer::SetTexelFormatOnRHIThread(DataFormat format)
{
    const bool         uniform   = m_usageFlags.HasFlag(RHIBufferUsageFlagBits::eTextureBuffer);
    const bool         storage   = m_usageFlags.HasFlag(RHIBufferUsageFlagBits::eImageBuffer);
    const uint32_t     texelSize = GetTextureFormatPixelSize(format);
    VkFormatProperties properties{};
    if (texelSize != 0)
    {
        vkGetPhysicalDeviceFormatProperties(m_pRHI->GetDevice()->GetPhysicalDeviceHandle(), ToVkFormat(format), &properties);
    }
    bool ready = (uniform || storage) && texelSize != 0 && m_requiredSize % texelSize == 0
              && m_requiredSize / texelSize <= m_pRHI->GetDevice()->GetPhysicalDeviceProperties().limits.maxTexelBufferElements
              && (!uniform || (properties.bufferFeatures & VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT) != 0)
              && (!storage || (properties.bufferFeatures & VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT) != 0);
    if (!ready)
    {
        ZEN_RHI_LOG_ERROR("Invalid texel buffer view: usage, format support, range alignment or element limit");
    }
    else if (m_bufferView == VK_NULL_HANDLE)
    {
        VkBufferViewCreateInfo bufferViewCI;

        InitVkStruct(bufferViewCI, VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO);

        bufferViewCI.buffer = m_vkBuffer;

        bufferViewCI.format = ToVkFormat(format);

        bufferViewCI.range  = m_requiredSize;

        bufferViewCI.offset = 0;

        VkBufferView bufferView{VK_NULL_HANDLE};

        const VkResult result = vkCreateBufferView(m_pRHI->GetVkDevice(), &bufferViewCI, nullptr, &bufferView);

        if (result != VK_SUCCESS)
        {
            ZEN_RHI_LOG_ERROR("vkCreateBufferView failed: {}", GetResultString(result));

            ready = false;
        }

        // Descriptors can retain this handle from recording through GPU completion.
        // Publish one immutable view only after creation succeeds.
        if (ready)
        {
            m_bufferView  = bufferView;

            m_texelFormat = format;
        }
    }
    else if (format != m_texelFormat)
    {
        ZEN_RHI_LOG_ERROR("Cannot change an existing buffer's texel format");

        ready = false;
    }

    return ready;
}
} // namespace zen
