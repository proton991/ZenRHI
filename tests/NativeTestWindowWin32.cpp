#include "NativeTestWindow.h"
#include "ZenRHI/Vulkan/VulkanSurface.h"
#include <windows.h>
#include "Vulkan/VulkanHeaders.h"
#include <vulkan/vulkan_win32.h>

namespace zen::test
{
namespace
{
VkResult CreateWin32Surface(VkInstance instance, void* window, VkSurfaceKHR* surface)
{
    const PFN_vkCreateWin32SurfaceKHR create =
        reinterpret_cast<PFN_vkCreateWin32SurfaceKHR>(vkGetInstanceProcAddr(instance, "vkCreateWin32SurfaceKHR"));
    VkResult result = VK_ERROR_EXTENSION_NOT_PRESENT;
    if (create)
    {
        VkWin32SurfaceCreateInfoKHR info{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
        info.hwnd      = static_cast<HWND>(window);
        info.hinstance = GetModuleHandleW(nullptr);
        result         = create(instance, &info, nullptr, surface);
    }
    return result;
}

constexpr VulkanSurfaceProvider kWin32Provider{CreateWin32Surface, nullptr};
} // namespace

NativeTestWindow::NativeTestWindow(uint32_t width, uint32_t height)
{
    m_window = CreateWindowExW(0, L"STATIC", L"ZenRHI test", WS_POPUP, 0, 0, static_cast<int>(width), static_cast<int>(height),
                               nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
}

NativeTestWindow::~NativeTestWindow()
{
    if (m_window)
    {
        DestroyWindow(static_cast<HWND>(m_window));
    }
}

RHIWindowHandle NativeTestWindow::GetHandle() const
{
    return {m_window, &kWin32Provider};
}

void NativeTestWindow::Resize(uint32_t width, uint32_t height)
{
    SetWindowPos(static_cast<HWND>(m_window), nullptr, 0, 0, static_cast<int>(width), static_cast<int>(height),
                 SWP_NOMOVE | SWP_NOACTIVATE | SWP_NOZORDER);
}
} // namespace zen::test
