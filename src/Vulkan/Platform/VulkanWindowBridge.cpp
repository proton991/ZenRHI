#include "VulkanWindowBridge.h"
#include "ZenRHI/Vulkan/VulkanSurface.h"
#include "Vulkan/VulkanCommon.h"
#if defined(ZEN_WIN32)
#    include <windows.h>
#    include <vulkan/vulkan_win32.h>
#elif defined(ZEN_MACOS)
#    include <vulkan/vulkan_metal.h>
#endif

namespace zen
{
void AddWindowInstanceExtensions(HeapVector<UniquePtr<VulkanInstanceExtension>>& extensions)
{
    const RHIOptions::SurfaceExtensions query = RHIOptions::GetInstance().surfaceExtensions;
    const std::span<const char* const>  names = query ? query() : std::span<const char* const>{};
    for (const char* name : names)
    {
        bool exists = false;

        for (const UniquePtr<VulkanInstanceExtension>& extension : extensions)
        {
            if (extension->GetName() == NameID(name))
            {
                extension->required = true;
                extension->SetEnable();
                exists = true;
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
    if (window.window && window.type == RHIWindowType::eCustom && window.provider && window.provider->create)
    {
        if (window.provider->checkOwner)
        {
            window.provider->checkOwner(window.window);
        }
        result = window.provider->create(instance, window.window, &surface);
    }
#if defined(ZEN_WIN32)
    else if (window.window && window.type == RHIWindowType::eWin32 && window.instance)
    {
        const PFN_vkCreateWin32SurfaceKHR create =
            reinterpret_cast<PFN_vkCreateWin32SurfaceKHR>(vkGetInstanceProcAddr(instance, "vkCreateWin32SurfaceKHR"));
        if (create)
        {
            VkWin32SurfaceCreateInfoKHR info{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
            info.hwnd      = static_cast<HWND>(window.window);
            info.hinstance = static_cast<HINSTANCE>(window.instance);
            result         = create(instance, &info, nullptr, &surface);
        }
    }
#elif defined(ZEN_MACOS)
    else if (window.window && window.type == RHIWindowType::eMetal)
    {
        const PFN_vkCreateMetalSurfaceEXT create =
            reinterpret_cast<PFN_vkCreateMetalSurfaceEXT>(vkGetInstanceProcAddr(instance, "vkCreateMetalSurfaceEXT"));
        if (create)
        {
            VkMetalSurfaceCreateInfoEXT info{VK_STRUCTURE_TYPE_METAL_SURFACE_CREATE_INFO_EXT};
            info.pLayer = static_cast<const CAMetalLayer*>(window.window);
            result      = create(instance, &info, nullptr, &surface);
        }
    }
#endif
    if (result == VK_SUCCESS && surface == VK_NULL_HANDLE)
    {
        result = VK_ERROR_INITIALIZATION_FAILED;
    }
    return result == VK_SUCCESS ? RHIResult<VkSurfaceKHR>(surface)
                                : RHIResult<VkSurfaceKHR>(MakeVulkanError(result, "CreateWindowSurface", __FILE__, __LINE__));
}
} // namespace zen
