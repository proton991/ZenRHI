#include "ZenRHI/RHIDiagnostics.h"
#include "ZenRHI/DynamicRHI.h"
#include "ZenRHI/RHIDebug.h"
#include "ZenRHI/Vulkan/VulkanRHIFactory.h"
#include "ZenFoundation/Utils/Errors.h"
#include "Vulkan/VulkanDebug.h"

namespace zen
{
RHIResult<std::vector<RHIAdapterInfo>> EnumerateRHIAdapters(const RHIDeviceDesc& description)
{
    return EnumerateVulkanAdapters(description);
}

RHIResult<DynamicRHI*> CreateRHIChecked(const RHIDeviceDesc& description)
{
    return CreateVulkanRHIChecked(description);
}

DynamicRHI* CreateRHI(const RHIDeviceDesc& description)
{
    return CreateVulkanRHI(description);
}

DynamicRHI* DynamicRHI::Create(RHIAPIType type)
{
    ZEN_RHI_VERIFY_MSG(type == RHIAPIType::eVulkan, "Unsupported graphics API");
    return CreateRHI(RHIOptions::GetInstance());
}

RHIDebug* RHIDebug::Create()
{
    RHIDebug* result{};

    if (GDynamicRHI != nullptr && GDynamicRHI->GetAPIType() == RHIAPIType::eVulkan)
    {
        result = ZEN_NEW() VulkanDebug();
    }
    else
    {
        ZEN_RHI_LOG_ERROR("RHI debug creation requires a supported, initialized backend");

        result = nullptr;
    }

    return result;
}

} // namespace zen
