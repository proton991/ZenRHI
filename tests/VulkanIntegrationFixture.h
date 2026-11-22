#pragma once
#include "TestShaderModule.h"
#include <filesystem>
#include "Vulkan/VulkanRHI.h"

namespace zen::test
{
// Set zero is reserved for the global heap in every Vulkan pipeline layout.
inline constexpr uint32_t kLocalResourceSet = kGlobalBindlessHeapIndex + 1;

inline void SetShaderStage(RHIShaderCreateInfo& info, RHIShaderStage stage, const char* file)
{
    info.stageFlags.SetFlag(RHIShaderStageToFlagBits(stage));
    zen::test::LoadShaderModule(info, stage, std::filesystem::path(RDG_REFLECTION_TEST_PATH) / file);
}

// Isolates each native Vulkan test device and its process-wide bindings.
class VulkanSession
{
public:
    VulkanSession()
    {
        GDynamicRHI = &rhi;
        rhi.Init();
    }

    ~VulkanSession()
    {
        rhi.Destroy();
        GDynamicRHI = nullptr;
        GVulkanRHI  = nullptr;
    }

    VulkanRHI rhi;
};

} // namespace zen::test
