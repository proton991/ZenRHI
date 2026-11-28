#include "ZenRHI/DynamicRHI.h"
#include "ZenRHI/RHIThread.h"
#include "ZenRHI/Vulkan/VulkanInterop.h"
#include "VulkanRHI.h"

namespace zen
{
namespace
{
RHIError InitializeBackend(const RHIDeviceDesc& description, VulkanRHI*& backend, std::vector<RHIAdapterInfo>* adapters)
{
    backend = ZEN_NEW() VulkanRHI(description);
    RHIError error =
        backend ? RHIError{} : MakeRHIError(RHIErrorCode::eOutOfHostMemory, "Allocate VulkanRHI", __FILE__, __LINE__);
    if (backend)
    {
        backend->GetRuntime().thread.SetStallReports(description.StallReportThreshold(), description.StallFatalThreshold(),
                                                     description.StallStackCapture());
        error = backend->InitChecked(adapters);
    }
    if (error.IsFailure() || adapters)
    {
        if (backend)
        {
            backend->Destroy();
        }
        ZEN_DELETE(backend);
        backend = nullptr;
    }
    else
    {
        RHIWorkerTimer::SetEnabled(description.WorkerTimingEnabled());
    }
    return error;
}
} // namespace

RHIResult<std::vector<RHIAdapterInfo>> EnumerateRHIAdapters(const RHIDeviceDesc& description)
{
    VulkanRHI*                  backend{};
    std::vector<RHIAdapterInfo> adapters;
    const RHIError              error = InitializeBackend(description, backend, &adapters);
    using Result                      = RHIResult<std::vector<RHIAdapterInfo>>;
    return error.IsFailure() ? Result(error) : Result(std::move(adapters));
}

RHIResult<DynamicRHI*> CreateRHIChecked(const RHIDeviceDesc& description)
{
    VulkanRHI*     backend{};
    const RHIError error = InitializeBackend(description, backend, nullptr);
    return error.IsFailure() ? RHIResult<DynamicRHI*>(error) : RHIResult<DynamicRHI*>(backend);
}

DynamicRHI* CreateRHI(const RHIDeviceDesc& description)
{
    RHIResult<DynamicRHI*> result = CreateRHIChecked(description);
    ZEN_RHI_VERIFY_MSG_F(result, "{} failed ({})", result.GetError().operation, result.GetError().nativeCode);
    return result.GetValue();
}

VulkanDeviceHandles GetVulkanDeviceHandles(const DynamicRHI& rhi)
{
    return {reinterpret_cast<VkInstance>(rhi.GetNativeObject(RHINativeObject::eInstance)),
            reinterpret_cast<VkPhysicalDevice>(rhi.GetNativeObject(RHINativeObject::ePhysicalDevice)),
            reinterpret_cast<VkDevice>(rhi.GetNativeObject(RHINativeObject::eDevice))};
}
} // namespace zen
