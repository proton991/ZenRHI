#include "VulkanWindowBridge.h"
#include "ZenRHI/Vulkan/VulkanSurface.h"
#include "Vulkan/VulkanCommon.h"

namespace zen
{
void AddWindowInstanceExtensions(HeapVector<UniquePtr<VulkanInstanceExtension>>& extensions,
                                 RHIDeviceDesc::SurfaceExtensions                query)
{
    const std::span<const char* const> names = query ? query() : std::span<const char* const>{};
    for (const char* name : names)
    {
        bool exists = false;

        for (const UniquePtr<VulkanInstanceExtension>& extension : extensions)
        {
            if (extension->GetName() == NameID(name))
            {
                extension->required = true;
                exists              = true;
                break;
            }
        }

        if (!exists)
        {
            extensions.emplace_back(MakeUnique<VulkanInstanceExtension>(name, true));
        }
    }
}

RHIResult<VkSurfaceKHR> CreateWindowSurface(VkInstance instance, RHIWindowHandle window)
{
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkResult     result  = VK_ERROR_INITIALIZATION_FAILED;
    if (window.window && window.provider && window.provider->create)
    {
        if (window.provider->checkOwner)
        {
            window.provider->checkOwner(window.window);
        }
        result = window.provider->create(instance, window.window, &surface);
    }
    if (result == VK_SUCCESS && surface == VK_NULL_HANDLE)
    {
        result = VK_ERROR_INITIALIZATION_FAILED;
    }
    return result == VK_SUCCESS ? RHIResult<VkSurfaceKHR>(surface)
                                : RHIResult<VkSurfaceKHR>(MakeVulkanError(result, "CreateWindowSurface", __FILE__, __LINE__));
}

void DestroyWindowSurface(VkInstance instance, VkSurfaceKHR surface)
{
    if (instance != VK_NULL_HANDLE && surface != VK_NULL_HANDLE)
    {
        vkDestroySurfaceKHR(instance, surface, nullptr);
    }
}
} // namespace zen
