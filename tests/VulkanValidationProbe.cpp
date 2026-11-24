#include "VulkanIntegrationFixture.h"
#include "Vulkan/VulkanBuffer.h"
#include "Vulkan/VulkanDevice.h"
#include "Vulkan/VulkanQueue.h"
#include "ZenRHI/TestSupport/Validation.h"
#include <gtest/gtest.h>

namespace zen
{
TEST(VulkanValidationTest, OverlappingBufferClears)
{
    test::VulkanSession session;
    RHIBufferCreateInfo desc{};
    desc.allocateType = RHIBufferAllocateType::eGPU;
    desc.size         = 64;
    desc.usageFlags.SetFlag(RHIBufferUsageFlagBits::eTransferDstBuffer);
    RHIResourcePtr<RHIBuffer> buffer = RHIResourcePtr<RHIBuffer>::Adopt(session.rhi.CreateBuffer(desc));
    ASSERT_NE(buffer.Get(), nullptr);
    const VkBuffer native          = static_cast<VulkanBuffer*>(buffer.Get())->GetVkBuffer();

    const VkDevice          device = session.rhi.GetVkDevice();
    VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    poolInfo.queueFamilyIndex = session.rhi.GetDevice()->GetGfxQueue()->GetFamilyIndex();
    VkCommandPool pool{};
    ASSERT_EQ(vkCreateCommandPool(device, &poolInfo, nullptr, &pool), VK_SUCCESS);
    for (bool corrected : {false, true})
    {
        VkCommandBufferAllocateInfo allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        allocate.commandBufferCount = 1;
        allocate.commandPool        = pool;
        allocate.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        VkCommandBuffer cmd{};
        ASSERT_EQ(vkAllocateCommandBuffers(device, &allocate, &cmd), VK_SUCCESS);
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        ASSERT_EQ(vkBeginCommandBuffer(cmd, &begin), VK_SUCCESS);
        const uint64_t before   = test::GetValidationCount(RHIMessageSeverity::eError);
        const uint64_t warnings = test::GetValidationCount(RHIMessageSeverity::eWarning);
        const uint64_t info     = test::GetValidationCount(RHIMessageSeverity::eInfo);
        vkCmdFillBuffer(cmd, native, 0, 64, 1);
        if (corrected)
        {
            VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
            barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &barrier, 0,
                                 nullptr, 0, nullptr);
        }
        vkCmdFillBuffer(cmd, native, 0, 64, 2);
        EXPECT_EQ(vkEndCommandBuffer(cmd), VK_SUCCESS);
        EXPECT_EQ(test::GetValidationCount(RHIMessageSeverity::eError) - before, corrected ? 0u : 1u);
        EXPECT_EQ(test::GetValidationCount(RHIMessageSeverity::eError, "SYNC-HAZARD-WRITE-AFTER-WRITE"), 1u);
        EXPECT_EQ(test::GetValidationCount(RHIMessageSeverity::eWarning), warnings);
        EXPECT_EQ(test::GetValidationCount(RHIMessageSeverity::eInfo), info);
        vkFreeCommandBuffers(device, pool, 1, &cmd);
    }
    vkDestroyCommandPool(device, pool, nullptr);
}
} // namespace zen
