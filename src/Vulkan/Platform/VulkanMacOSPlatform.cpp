#include "ZenFoundation/Utils/Errors.h"
#include "VulkanWindowBridge.h"
#include "Vulkan/VulkanRHI.h"
#if defined(ZEN_MACOS)
#    include "Vulkan/Platform/VulkanMacOSPlatform.h"
#    include "Vulkan/VulkanExtension.h"

namespace zen
{
void VulkanMacOSPlatform::AddInstanceExtensions(HeapVector<UniquePtr<VulkanInstanceExtension>>& extensions)
{
#    if defined(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME)
    extensions.emplace_back(MakeUnique<VulkanInstanceExtension>(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME));
#    endif
    extensions.emplace_back(MakeUnique<VulkanInstanceExtension>("VK_EXT_metal_surface", true));

    AddWindowInstanceExtensions(extensions);
}

RHIResult<VkSurfaceKHR> VulkanMacOSPlatform::CreateSurface(VkInstance instance, RHIWindowHandle window)
{
    return CreateWindowSurface(instance, window);
}

void VulkanMacOSPlatform::DestroySurface(VkInstance instance, VkSurfaceKHR surface)
{
    if (instance != VK_NULL_HANDLE && surface != VK_NULL_HANDLE)
    {
        vkDestroySurfaceKHR(instance, surface, nullptr);
    }
}
} // namespace zen

#endif
