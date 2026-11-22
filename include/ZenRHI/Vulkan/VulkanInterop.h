#pragma once
#include <vulkan/vulkan_core.h>

namespace zen
{
struct VulkanDeviceHandles
{
    VkInstance       instance{};
    VkPhysicalDevice physicalDevice{};
    VkDevice         device{};
};

// Borrowed handles for the active device. Query after initialization and before starting teardown.
VulkanDeviceHandles GetVulkanDeviceHandles();
} // namespace zen
