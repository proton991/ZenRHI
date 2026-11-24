#include <ZenRHI/RHICommandListExecutor.h>
#include <ZenRHI/Vulkan/VulkanInterop.h>
#include <cstdlib>

int main(int argc, char** argv)
{
    int status = 0;
    if (argc > 1)
    {
        zen::RHIDeviceDesc desc;
        desc.SetValidationMode(zen::RHIValidationMode::eRequired);
#if defined(_WIN32)
        const bool        unsupported = std::string_view(argv[1]) == "--unsupported-profile";
        const char*       setting     = unsupported ? "VK_KHRONOS_PROFILES_EXCLUDE_DEVICE_EXTENSIONS" : "VK_LAYER_PATH";
        const char*       previous    = std::getenv(setting);
        const std::string saved       = previous ? previous : "";
        _putenv_s(setting, unsupported ? "VK_KHR_dynamic_rendering" : "Z:/no-validation-manifests");
        if (unsupported)
        {
            zen::RHIResult<std::vector<zen::RHIAdapterInfo>> report = zen::EnumerateRHIAdapters(desc);
            if (report && !report.GetValue().empty())
            {
                for (const zen::RHIAdapterInfo& adapter : report.GetValue())
                {
                    if (adapter.unsupportedReason.find("VK_KHR_dynamic_rendering") == std::string::npos)
                    {
                        status = 1;
                    }
                }
            }
            else
            {
                status = 1;
            }
        }
        zen::RHIResult<zen::DynamicRHI*> failure = zen::CreateRHIChecked(desc);
        _putenv_s(setting, saved.c_str());
        if (failure || !failure.GetError().operation || zen::GetVulkanDeviceHandles().device || GDynamicRHI
            || (unsupported && failure.GetError().code != zen::RHIErrorCode::eUnsupported)
            || (!unsupported && failure.GetError().nativeCode != VK_ERROR_LAYER_NOT_PRESENT))
        {
            status = 1;
        }
        else
        {
            std::fprintf(stderr, "Host survived %s (%lld)\n", failure.GetError().operation,
                         static_cast<long long>(failure.GetError().nativeCode));
        }
#endif
        zen::RHIResult<zen::DynamicRHI*> created = zen::CreateRHIChecked(desc);
        if (created)
        {
            zen::RHICommandListExecutor executor(created.GetValue(), zen::RHIExecutionMode::eThreaded);
            executor.Destroy();
        }
        else
        {
            status = 1;
        }
    }
    return status;
}
