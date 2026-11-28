#pragma once
#include <vulkan/vulkan_core.h>

namespace zen
{
class DynamicRHI;

struct VulkanDeviceHandles
{
    VkInstance       instance{};
    VkPhysicalDevice physicalDevice{};
    VkDevice         device{};
};

// Borrowed handles of the device behind rhi; null before initialization. Valid until teardown starts.
VulkanDeviceHandles GetVulkanDeviceHandles(const DynamicRHI& rhi);
} // namespace zen
