#pragma once
#include "TestShaderModule.h"
#include "VulkanIntegrationFixture.h"
#include "ZenRHI/TestSupport/ScopedVulkanCall.h"
#include "Vulkan/VulkanBuffer.h"
#include "Vulkan/VulkanCommandList.h"
#include "Vulkan/VulkanDevice.h"
#include "Vulkan/VulkanPipeline.h"
#include "Vulkan/VulkanTexture.h"
#include <gtest/gtest.h>
#include <array>
#include <bit>
#include <filesystem>
#include <functional>
#include <memory>
#include <vector>

namespace
{
using namespace zen;

// Inspect the public Vulkan boundary and forward creation to the real driver.
struct PipelineObserver
{
    static inline PFN_vkCreateGraphicsPipelines                            create;
    static inline std::function<void(const VkGraphicsPipelineCreateInfo&)> inspect;

    static VKAPI_ATTR VkResult VKAPI_CALL Create(VkDevice                            device,
                                                 VkPipelineCache                     cache,
                                                 uint32_t                            count,
                                                 const VkGraphicsPipelineCreateInfo* infos,
                                                 const VkAllocationCallbacks*        allocator,
                                                 VkPipeline*                         pipelines)
    {
        for (uint32_t i = 0; i < count; ++i)
        {
            if (inspect)
            {
                inspect(infos[i]);
            }
        }

        return create(device, cache, count, infos, allocator, pipelines);
    }

    static VKAPI_ATTR VkBool32 VKAPI_CALL Validation(VkDebugUtilsMessageSeverityFlagBitsEXT,
                                                     VkDebugUtilsMessageTypeFlagsEXT,
                                                     const VkDebugUtilsMessengerCallbackDataEXT* data,
                                                     void*)
    {
        ADD_FAILURE() << data->pMessage;

        return VK_FALSE;
    }
};

class VulkanPipelineIntegrationTest : public testing::Test
{
protected:
    std::unique_ptr<test::VulkanSession>                                   session;
    std::unique_ptr<test::ScopedVulkanCall<PFN_vkCreateGraphicsPipelines>> observe;
    VulkanCommandListContext*                                              context{};
    VkDebugUtilsMessengerEXT                                               messenger{};
    std::vector<RHIShader*>                                                shaders;
    std::vector<RHIPipeline*>                                              pipelines;
    std::vector<RHITexture*>                                               textures;
    std::vector<RHIBuffer*>                                                buffers;

    void SetUp() override
    {
        session                   = std::make_unique<test::VulkanSession>();

        PipelineObserver::create  = vkCreateGraphicsPipelines;

        PipelineObserver::inspect = {};

        observe = std::make_unique<test::ScopedVulkanCall<PFN_vkCreateGraphicsPipelines>>(vkCreateGraphicsPipelines,
                                                                                          PipelineObserver::Create);

        VkDebugUtilsMessengerCreateInfoEXT info{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};

        info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;

        info.messageType     = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT;

        info.pfnUserCallback = PipelineObserver::Validation;

        ASSERT_EQ(vkCreateDebugUtilsMessengerEXT(session->rhi.GetInstance(), &info, nullptr, &messenger), VK_SUCCESS);

        context = static_cast<VulkanCommandListContext*>(session->rhi.GetCommandContext(RHICommandContextType::eGraphics));
    }

    void TearDown() override
    {
        session->rhi.WaitDeviceIdle();

        ZEN_DELETE(context);

        for (zen::RHIPipeline* pipeline : pipelines)
        {
            session->rhi.DestroyPipeline(pipeline);
        }

        for (zen::RHIShader* shader : shaders)
        {
            session->rhi.DestroyShader(shader);
        }

        for (zen::RHITexture* texture : textures)
        {
            session->rhi.DestroyTexture(texture);
        }

        for (zen::RHIBuffer* buffer : buffers)
        {
            session->rhi.DestroyBuffer(buffer);
        }

        observe.reset();

        PipelineObserver::inspect = {};

        vkDestroyDebugUtilsMessengerEXT(session->rhi.GetInstance(), messenger, nullptr);

        session.reset();
    }

    static void Stage(RHIShaderCreateInfo& info, RHIShaderStage stage, const char* file)
    {
        info.stageFlags.SetFlag(RHIShaderStageToFlagBits(stage));

        zen::test::LoadShaderModule(info, stage, std::filesystem::path(ZENRHI_TEST_SHADER_DIR) / file);
    }

    VulkanShader* Shader(bool                                                   compute   = false,
                         bool                                                   depthOnly = false,
                         const HashMap<uint32_t, RHIShaderSpecializationValue>& constants = {})
    {
        RHIShaderCreateInfo info{};

        info.specializationConstants.assign(constants.begin(), constants.end());

        if (compute)
        {
            Stage(info, RHIShaderStage::eCompute, "pipeline_specialization.comp.spv");
        }
        else
        {
            Stage(info, RHIShaderStage::eVertex, "pipeline.vert.spv");

            Stage(info, RHIShaderStage::eFragment, depthOnly ? "pipeline_depth.frag.spv" : "pipeline.frag.spv");
        }

        VulkanShader* shader = static_cast<VulkanShader*>(session->rhi.CreateShader(info));

        shaders.push_back(shader);

        return shader;
    }

    RHIPipeline* Graphics(RHIShader* shader, const RHIRenderingLayout& layout, const RHIGfxPipelineStates& states = {})
    {
        RHIGfxPipelineCreateInfo info{};

        info.pShader          = shader;

        info.pRenderingLayout = &layout;

        info.states           = states;

        RHIPipeline* pipeline = session->rhi.CreatePipeline(info);

        pipelines.push_back(pipeline);

        return pipeline;
    }

    VulkanTexture* Texture(DataFormat format, RHITextureUsageFlagBits usage, bool transfer = true)
    {
        RHITextureCreateInfo info{};

        info.type   = RHITextureType::e2D;

        info.format = format;

        info.width = info.height = 4;

        info.usageFlags.SetFlag(usage);

        if (transfer)
        {
            info.usageFlags.SetFlag(RHITextureUsageFlagBits::eTransferSrc);
        }

        VulkanTexture* texture = static_cast<VulkanTexture*>(session->rhi.CreateTexture(info));

        textures.push_back(texture);

        return texture;
    }

    VulkanBuffer* Buffer()
    {
        RHIBufferCreateInfo info{};

        info.size         = 128;

        info.allocateType = RHIBufferAllocateType::eCPURead;

        info.usageFlags.SetFlags(RHIBufferUsageFlagBits::eStorageBuffer, RHIBufferUsageFlagBits::eTransferDstBuffer);

        VulkanBuffer* buffer = static_cast<VulkanBuffer*>(session->rhi.CreateBuffer(info));

        buffers.push_back(buffer);

        return buffer;
    }

    VkCommandBuffer Commands()
    {
        return context->GetCommandBuffer()->GetVkHandle();
    }

    void Transition(VulkanTexture*       texture,
                    VkImageLayout        before,
                    VkImageLayout        after,
                    VkAccessFlags        srcAccess,
                    VkAccessFlags        dstAccess,
                    VkPipelineStageFlags srcStage,
                    VkPipelineStageFlags dstStage)
    {
        VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};

        barrier.srcAccessMask       = srcAccess;

        barrier.dstAccessMask       = dstAccess;

        barrier.oldLayout           = before;

        barrier.newLayout           = after;

        barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;

        barrier.image                                             = texture->GetVkImage();

        barrier.subresourceRange                                  = texture->GetVkSubresourceRange();

        vkCmdPipelineBarrier(Commands(), srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    }

    void Copy(VulkanTexture* texture, VulkanBuffer* buffer, VkImageAspectFlags aspect, VkDeviceSize offset = 0)
    {
        VkBufferImageCopy copy{};

        copy.bufferOffset     = offset;

        copy.imageSubresource = {aspect, 0, 0, 1};

        copy.imageExtent      = {4, 4, 1};

        vkCmdCopyImageToBuffer(Commands(), texture->GetVkImage(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer->GetVkBuffer(),
                               1, &copy);
    }

    void SubmitAndWait(VkPipelineStageFlags stage, VkAccessFlags access)
    {
        VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};

        barrier.srcAccessMask = access;

        barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;

        vkCmdPipelineBarrier(Commands(), stage, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);

        HeapVector<VulkanWorkload*> workloads;

        context->CollectWorkloads(workloads);

        VulkanQueue* queue = session->rhi.GetDevice()->GetGfxQueue();

        for (zen::VulkanWorkload* workload : workloads)
        {
            queue->EnqueueWorkload(workload);
        }

        uint64_t serial = 0;

        ASSERT_EQ(queue->SubmitPendingWorkloads(serial), RHISubmissionResult::eSuccess);

        ASSERT_TRUE(queue->WaitForCompletion(serial, UINT64_MAX));
    }
};

} // namespace
