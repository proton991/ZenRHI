#pragma once

namespace zen
{
struct VulkanSurfaceProvider;

// A borrowed window to present to. The provider creates its Vulkan surface, so the RHI carries no window-system
// code; the host keeps both alive until the viewport is destroyed.
struct RHIWindowHandle
{
    void*                        window{};
    const VulkanSurfaceProvider* provider{};
};
} // namespace zen
