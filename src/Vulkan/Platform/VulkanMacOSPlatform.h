#pragma once
#include "VulkanPlatformCommon.h"
#include "ZenFoundation/Templates/HeapVector.h"

#if defined(ZEN_MACOS)

#    include <vector>
#    include "ZenFoundation/Utils/UniquePtr.h"
#    define VK_USE_PLATFORM_MACOS_MVK

namespace zen
{
class VulkanRHI;
class VulkanInstanceExtension;
class VulkanMacOSPlatform
{
public:
    static void AddInstanceExtensions(HeapVector<UniquePtr<VulkanInstanceExtension>>& extensions);

    static RHIResult<VkSurfaceKHR> CreateSurface(VkInstance instance, RHIWindowHandle window);

    static void DestroySurface(VkInstance instance, VkSurfaceKHR surface);
};

typedef VulkanMacOSPlatform VulkanPlatform;
} // namespace zen

#endif
