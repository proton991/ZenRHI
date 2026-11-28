#pragma once
#include "Vulkan/VulkanExtension.h"
#include "ZenRHI/RHIWindowHandle.h"
#include "ZenRHI/RHIError.h"
#include "ZenRHI/RHIDeviceDesc.h"
namespace zen
{
void AddWindowInstanceExtensions(HeapVector<UniquePtr<VulkanInstanceExtension>>& extensions,
                                 RHIDeviceDesc::SurfaceExtensions                query);

// The window's provider creates the surface. A null window or provider fails.
RHIResult<VkSurfaceKHR> CreateWindowSurface(VkInstance instance, RHIWindowHandle window);

void DestroyWindowSurface(VkInstance instance, VkSurfaceKHR surface);
} // namespace zen
