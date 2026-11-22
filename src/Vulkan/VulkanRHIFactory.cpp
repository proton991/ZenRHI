#include "ZenRHI/Vulkan/VulkanRHIFactory.h"
#include "ZenRHI/Vulkan/VulkanInterop.h"
#include "VulkanRHI.h"

namespace zen
{
DynamicRHI* CreateVulkanRHI(const RHIDeviceDesc& description)
{
    SetRHIMessageCallback(description.messageCallback);
    RHIOptions::GetInstance().Configure(description);
    DynamicRHI* backend = ZEN_NEW() VulkanRHI();
    backend->Init();
    GDynamicRHI = backend;
    return backend;
}

VulkanDeviceHandles GetVulkanDeviceHandles()
{
    VulkanDeviceHandles handles;
    if (GVulkanRHI && GVulkanRHI->GetDevice())
    {
        handles = {GVulkanRHI->GetInstance(), GVulkanRHI->GetPhysicalDevice(), GVulkanRHI->GetVkDevice()};
    }
    return handles;
}
} // namespace zen
