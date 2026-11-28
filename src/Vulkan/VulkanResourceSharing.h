#pragma once
#include "VulkanDevice.h"

namespace zen
{
// Graphics and compute families, plus transfer when the resource has transfer usage, without repeats.
inline VulkanQueueSharing MakeQueueSharing(uint32_t graphics, uint32_t compute, uint32_t transfer, bool transferUsage)
{
    VulkanQueueSharing sharing;
    sharing.families[0] = graphics;
    sharing.count       = 1;
    if (compute != graphics)
    {
        sharing.families[sharing.count++] = compute;
    }
    if (transferUsage && transfer != graphics && transfer != compute)
    {
        sharing.families[sharing.count++] = transfer;
    }
    return sharing;
}

// RHI resources can be used on graphics and compute without queue-family ownership transfers.
// Transfer usage additionally permits the dedicated transfer queue. Synchronization between
// accesses on different queues is still the caller's responsibility.
// The allocator consumes create-info synchronously. The family list is cleared before returning so
// a cached create-info does not keep pointing at the device.
template <typename CreateInfo, typename Allocate>
void AllocateWithQueueSharing(CreateInfo& info, const VulkanQueueSharing& sharing, Allocate&& allocate)
{
    const bool concurrent      = sharing.count > 1;
    info.sharingMode           = concurrent ? VK_SHARING_MODE_CONCURRENT : VK_SHARING_MODE_EXCLUSIVE;
    info.queueFamilyIndexCount = concurrent ? sharing.count : 0;
    info.pQueueFamilyIndices   = concurrent ? sharing.families.data() : nullptr;
    allocate();
    info.queueFamilyIndexCount = 0;
    info.pQueueFamilyIndices   = nullptr;
}
} // namespace zen
