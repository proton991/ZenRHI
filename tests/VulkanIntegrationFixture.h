#pragma once
#include "TestShaderModule.h"
#include "NativeTestWindow.h"
#include <cstdlib>
#include <filesystem>
#include "Vulkan/VulkanRHI.h"

namespace zen
{
struct VulkanRHITestAccess
{
    // Installs an allocator that observes later allocations; returns the previous one.
    static VulkanMemoryAllocator* ReplaceMemoryAllocator(VulkanRHI& rhi, VulkanMemoryAllocator* pAllocator)
    {
        VulkanMemoryAllocator* previous = rhi.m_pMemoryAllocator;
        rhi.m_pMemoryAllocator          = pAllocator;
        return previous;
    }

    // Tests that build extra devices or managers on a live session vary the options those objects read.
    static RHIDeviceDesc& DeviceDesc(VulkanRHI& rhi)
    {
        return rhi.m_desc;
    }
};
} // namespace zen

namespace zen::test
{
// Native test setup aborts on a startup failure, as the production factory reports it.
template <typename T> void InitOrAbort(T& object)
{
    const RHIError error = object.InitChecked();
    ZEN_RHI_VERIFY_MSG_F(!error.IsFailure(), "{} failed ({})", error.operation, error.nativeCode);
}

// Every native test device requires validation. ZEN_VULKAN_DEVICE picks the adapter on multi-GPU machines.
inline RHIDeviceDesc MakeTestDeviceDesc()
{
    RHIDeviceDesc desc;
    desc.SetValidationMode(RHIValidationMode::eRequired);
    desc.surfaceExtensions = &NativeTestWindow::InstanceExtensions;
    if (const char* device = std::getenv("ZEN_VULKAN_DEVICE"))
    {
        desc.SetPreferredDevice(device);
    }
    return desc;
}

// Set zero is reserved for the global heap in every Vulkan pipeline layout.
inline constexpr uint32_t kLocalResourceSet = kGlobalBindlessHeapIndex + 1;

inline void SetShaderStage(RHIShaderCreateInfo& info, RHIShaderStage stage, const char* file)
{
    info.stageFlags.SetFlag(RHIShaderStageToFlagBits(stage));
    zen::test::LoadShaderModule(info, stage, std::filesystem::path(ZENRHI_TEST_SHADER_DIR) / file);
}

// Isolates each native Vulkan test device.
class VulkanSession
{
public:
    explicit VulkanSession(const RHIDeviceDesc& desc = MakeTestDeviceDesc()) : rhi(desc)
    {
        InitOrAbort(rhi);
    }

    ~VulkanSession()
    {
        rhi.Destroy();
    }

    VulkanRHI rhi;
};

} // namespace zen::test
