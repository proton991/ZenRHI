#pragma once
#include "Vulkan/VulkanExtension.h"
#include "ZenRHI/RHIWindowHandle.h"
#include "ZenRHI/RHIError.h"
namespace zen
{
void AddWindowInstanceExtensions(HeapVector<UniquePtr<VulkanInstanceExtension>>& extensions);

RHIResult<VkSurfaceKHR> CreateWindowSurface(VkInstance instance, RHIWindowHandle window);
} // namespace zen
