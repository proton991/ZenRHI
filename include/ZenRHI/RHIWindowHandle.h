#pragma once

namespace zen
{
struct VulkanSurfaceProvider;
enum class RHIWindowType
{
    eWin32,
    eMetal,
    eCustom
};

struct RHIWindowHandle
{
    RHIWindowType                type{RHIWindowType::eCustom};
    void*                        window{};   // HWND, CAMetalLayer*, or borrowed custom provider context
    void*                        instance{}; // HINSTANCE for Win32
    const VulkanSurfaceProvider* provider{};
};
} // namespace zen
