#include "ZenRHI/Vulkan/VulkanRHIFactory.h"
#include "ZenRHI/Vulkan/VulkanInterop.h"
#include "VulkanRHI.h"

namespace zen
{
namespace
{
RHIError InitializeBackend(const RHIDeviceDesc& description, VulkanRHI*& backend, std::vector<RHIAdapterInfo>* adapters)
{
    RHIError error;
    if (GVulkanRHI || GDynamicRHI)
    {
        error = MakeRHIError(RHIErrorCode::eInvalidArgument, "Vulkan backend already active", __FILE__, __LINE__);
    }
    else
    {
        const RHIDeviceDesc      previous = RHIOptions::GetInstance();
        const RHIMessageCallback callback = GetRHIMessageCallback();
        SetRHIMessageCallback(description.messageCallback);
        RHIOptions::GetInstance().Configure(description);
        backend = ZEN_NEW() VulkanRHI();
        error   = backend ? backend->InitChecked(adapters)
                          : MakeRHIError(RHIErrorCode::eOutOfHostMemory, "Allocate VulkanRHI", __FILE__, __LINE__);
        if (error.IsFailure() || adapters)
        {
            if (backend)
            {
                backend->Destroy();
            }
            ZEN_DELETE(backend);
            backend = nullptr;
            RHIOptions::GetInstance().Configure(previous);
            SetRHIMessageCallback(callback);
        }
        else
        {
            GDynamicRHI = backend;
        }
    }
    return error;
}
} // namespace

RHIResult<std::vector<RHIAdapterInfo>> EnumerateVulkanAdapters(const RHIDeviceDesc& description)
{
    VulkanRHI*                  backend{};
    std::vector<RHIAdapterInfo> adapters;
    const RHIError              error = InitializeBackend(description, backend, &adapters);
    using Result                      = RHIResult<std::vector<RHIAdapterInfo>>;
    return error.IsFailure() ? Result(error) : Result(std::move(adapters));
}

RHIResult<DynamicRHI*> CreateVulkanRHIChecked(const RHIDeviceDesc& description)
{
    VulkanRHI*     backend{};
    const RHIError error = InitializeBackend(description, backend, nullptr);
    return error.IsFailure() ? RHIResult<DynamicRHI*>(error) : RHIResult<DynamicRHI*>(backend);
}

DynamicRHI* CreateVulkanRHI(const RHIDeviceDesc& description)
{
    RHIResult<DynamicRHI*> result = CreateVulkanRHIChecked(description);
    ZEN_RHI_VERIFY_MSG_F(result, "{} failed ({})", result.GetError().operation, result.GetError().nativeCode);
    return result.GetValue();
}

VulkanDeviceHandles GetVulkanDeviceHandles()
{
    VulkanDeviceHandles handles;
    if (GVulkanRHI && GVulkanRHI->IsInitialized())
    {
        handles = {GVulkanRHI->GetInstance(), GVulkanRHI->GetPhysicalDevice(), GVulkanRHI->GetVkDevice()};
    }
    return handles;
}
} // namespace zen
