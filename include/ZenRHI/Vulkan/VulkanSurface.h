#pragma once
#include <vulkan/vulkan_core.h>

namespace zen
{
struct VulkanSurfaceProvider
{
    VkResult (*create)(VkInstance instance, void* window, VkSurfaceKHR* surface);
    void     (*checkOwner)(void* window);
};
} // namespace zen
