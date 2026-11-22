#include "Vulkan/VulkanTypes.h"
#include "Vulkan/VulkanDebug.h"
#include "Vulkan/VulkanDevice.h"
#include "Vulkan/VulkanRHI.h"
#include "Vulkan/VulkanTexture.h"
#include "Vulkan/VulkanPipeline.h"

namespace zen
{
VulkanDebug::VulkanDebug() {}

void VulkanDebug::SetPipelineDebugName(RHIPipeline* pipeline, std::string_view name)
{
    if (pipeline != nullptr)
    {
        GVulkanRHI->GetDevice()->SetObjectName(VK_OBJECT_TYPE_PIPELINE,
                                               reinterpret_cast<uint64_t>(TO_VK_PIPELINE(pipeline)->GetVkPipeline()), name);
    }
}

void VulkanDebug::SetTextureDebugName(RHITexture* texture, std::string_view name)
{
    if (texture != nullptr)
    {
        GVulkanRHI->GetDevice()->SetObjectName(VK_OBJECT_TYPE_IMAGE,
                                               reinterpret_cast<uint64_t>(TO_VK_TEXTURE(texture)->GetVkImage()), name);
    }
}
} // namespace zen
