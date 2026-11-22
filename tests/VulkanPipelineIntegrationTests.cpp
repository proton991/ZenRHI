#include "VulkanPipelineTestFixture.h"

namespace
{
TEST_F(VulkanPipelineIntegrationTest, SpecializationPayloadSurvivesShaderInitAndDispatchesTypedValues)
{
    // Create several shaders before their pipelines so temporary reflection storage is reused.
    VulkanShader* defaults   = Shader(true);

    VulkanShader* overridden = Shader(true, false, {{0, false}, {1, -19}, {2, 4.5f}});

    VulkanShader* enabled    = Shader(true, false, {{0, true}, {1, 23}, {2, -2.75f}});

    const std::array<VulkanShader*, 3> inputs{defaults, overridden, enabled};

    const std::array<std::array<uint32_t, 3>, 3> expected{{{1u, static_cast<uint32_t>(-7), std::bit_cast<uint32_t>(1.25f)},
                                                           {0u, static_cast<uint32_t>(-19), std::bit_cast<uint32_t>(4.5f)},
                                                           {1u, 23u, std::bit_cast<uint32_t>(-2.75f)}}};

    for (size_t i = 0; i < inputs.size(); ++i)
    {
        SCOPED_TRACE(i);

        const VkSpecializationInfo* info = inputs[i]->GetStageCreateInfoData()[0].pSpecializationInfo;

        ASSERT_NE(info, nullptr);

        ASSERT_EQ(info->mapEntryCount, 3u);

        ASSERT_EQ(info->dataSize, 12u);

        std::array<bool, 3> seen{};

        for (uint32_t j = 0; j < info->mapEntryCount; ++j)
        {
            const VkSpecializationMapEntry& entry = info->pMapEntries[j];

            ASSERT_LT(entry.constantID, 3u);

            ASSERT_EQ(entry.size, sizeof(uint32_t));

            ASSERT_LE(entry.offset + entry.size, info->dataSize);

            EXPECT_FALSE(seen[entry.constantID]);

            seen[entry.constantID] = true;

            uint32_t bits          = 0;

            memcpy(&bits, static_cast<const uint8_t*>(info->pData) + entry.offset, entry.size);

            EXPECT_EQ(bits, expected[i][entry.constantID]);
        }

        RHIPipeline* pipeline = session->rhi.CreatePipeline(RHIComputePipelineCreateInfo{inputs[i]});

        pipelines.push_back(pipeline);

        RHIBuffer* buffer = Buffer();

        context->RHIBindPipeline(pipeline);

        RHIBatchedShaderParameters parameters;

        parameters.AddResourceParam(*inputs[i]->GetSRDByLocation(test::kLocalResourceSet, 0), buffer, nullptr, 0);

        context->RHISetShaderParameters(parameters);

        context->RHIDispatch(1, 1, 1);
    }

    SubmitAndWait(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT);

    for (size_t i = 0; i < inputs.size(); ++i)
    {
        const uint32_t* values = reinterpret_cast<const uint32_t*>(buffers[i]->Map());

        for (size_t j = 0; j < 3; ++j)
        {
            EXPECT_EQ(values[j], expected[i][j]);
        }

        buffers[i]->Unmap();
    }
}

TEST_F(VulkanPipelineIntegrationTest, EmptyFragmentModuleRejectsCreation)
{
    RHIShaderCreateInfo info{};

    Stage(info, RHIShaderStage::eVertex, "pipeline.vert.spv");

    info.spirv->SetStageSPIRV(RHIShaderStage::eFragment, {});

    RHIShader* shader = session->rhi.CreateShader(info);

    EXPECT_EQ(shader, nullptr);

    if (shader != nullptr)
    {
        shaders.push_back(shader);
    }
}

TEST_F(VulkanPipelineIntegrationTest, AllDynamicStateMasksProduceValidPipelines)
{
    VulkanShader* shader = Shader();

    RHIRenderingLayout layout{};

    layout.SetRenderArea(2, 3, 17, 19);

    layout.numColorRenderTargets        = 1;

    layout.colorRenderTargets[0].format = DataFormat::eR8G8B8A8UNORM;

    const VkDynamicState states[]       = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_LINE_WIDTH,
                                           VK_DYNAMIC_STATE_DEPTH_BIAS};

    for (uint32_t mask = 0; mask < 16; ++mask)
    {
        SCOPED_TRACE(mask);

        RHIGfxPipelineStates info{};

        info.colorBlendState.AddAttachment();

        std::vector<VkDynamicState> expected;

        for (uint32_t bit = 0; bit < 4; ++bit)
        {
            if (mask & (1u << bit))
            {
                info.dynamicStates.Enable(static_cast<RHIDynamicState>(bit));

                expected.push_back(states[bit]);
            }
        }

        PipelineObserver::inspect = [&](const VkGraphicsPipelineCreateInfo& ci) {
            const VkPipelineDynamicStateCreateInfo& dynamic = *ci.pDynamicState;

            ASSERT_EQ(dynamic.dynamicStateCount, expected.size());

            for (size_t i = 0; i < expected.size(); ++i)
            {
                EXPECT_EQ(dynamic.pDynamicStates[i], expected[i]);
            }

            if (!(mask & 1u))
            {
                ASSERT_NE(ci.pViewportState->pViewports, nullptr);

                const VkViewport& viewport = ci.pViewportState->pViewports[0];

                EXPECT_FLOAT_EQ(viewport.x, 2.0f);

                EXPECT_FLOAT_EQ(viewport.y, 3.0f);

                EXPECT_FLOAT_EQ(viewport.width, 17.0f);

                EXPECT_FLOAT_EQ(viewport.height, 19.0f);

                EXPECT_FLOAT_EQ(viewport.minDepth, 0.0f);

                EXPECT_FLOAT_EQ(viewport.maxDepth, 1.0f);
            }

            if (!(mask & 2u))
            {
                ASSERT_NE(ci.pViewportState->pScissors, nullptr);

                const VkRect2D& scissor = ci.pViewportState->pScissors[0];

                EXPECT_EQ(scissor.offset.x, 2);

                EXPECT_EQ(scissor.offset.y, 3);

                EXPECT_EQ(scissor.extent.width, 17u);

                EXPECT_EQ(scissor.extent.height, 19u);
            }
        };

        Graphics(shader, layout, info);
    }
}

TEST_F(VulkanPipelineIntegrationTest, IndependentColorAndAlphaFactorsProduceExpectedPixels)
{
    VulkanTexture* texture = Texture(DataFormat::eR8G8B8A8UNORM, RHITextureUsageFlagBits::eColorAttachment);

    VulkanBuffer* buffer   = Buffer();

    RHIRenderingLayout layout{};

    layout.SetRenderArea(0, 0, 4, 4);

    layout.AddColorRenderTarget(DataFormat::eR8G8B8A8UNORM, texture, RHIRenderTargetLoadOp::eClear,
                                RHIRenderTargetStoreOp::eStore, RHIRenderTargetClearValue(Color(0, 0, 1, 1)));

    RHIGfxPipelineStates states{};

    states.colorBlendState.AddAttachment();

    RHIGfxPipelineColorBlendState::Attachment& blend = states.colorBlendState.attachments[0];

    blend.enableBlend                                = true;

    blend.srcColorBlendFactor                        = RHIBlendFactor::eSrcAlpha;

    blend.dstColorBlendFactor                        = RHIBlendFactor::eOneMinusSrcAlpha;

    blend.srcAlphaBlendFactor                        = RHIBlendFactor::eOne;

    blend.dstAlphaBlendFactor                        = RHIBlendFactor::eZero;

    PipelineObserver::inspect                        = [](const VkGraphicsPipelineCreateInfo& ci) {
        ASSERT_EQ(ci.pColorBlendState->attachmentCount, 1u);

        const VkPipelineColorBlendAttachmentState& blend = ci.pColorBlendState->pAttachments[0];

        EXPECT_EQ(blend.srcColorBlendFactor, VK_BLEND_FACTOR_SRC_ALPHA);

        EXPECT_EQ(blend.dstColorBlendFactor, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA);

        EXPECT_EQ(blend.srcAlphaBlendFactor, VK_BLEND_FACTOR_ONE);

        EXPECT_EQ(blend.dstAlphaBlendFactor, VK_BLEND_FACTOR_ZERO);
    };

    RHIPipeline* pipeline = Graphics(Shader(), layout, states);

    Transition(texture, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0,
               VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
               VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);

    context->RHIBeginRendering(&layout);

    context->RHIBindPipeline(pipeline);

    context->RHIDraw(3, 1, 0, 0);

    context->RHIEndRendering();

    Transition(texture, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
               VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
               VK_PIPELINE_STAGE_TRANSFER_BIT);

    Copy(texture, buffer, VK_IMAGE_ASPECT_COLOR_BIT);

    SubmitAndWait(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);

    const uint8_t* pixels = buffer->Map();

    for (uint32_t i = 0; i < 16; ++i)
    {
        SCOPED_TRACE(i);

        EXPECT_NEAR(pixels[i * 4], 64, 1);

        EXPECT_EQ(pixels[i * 4 + 1], 0);

        EXPECT_NEAR(pixels[i * 4 + 2], 191, 1);

        EXPECT_NEAR(pixels[i * 4 + 3], 64, 1);
    }

    buffer->Unmap();
}

TEST_F(VulkanPipelineIntegrationTest, MultisampleMasksProduceExpectedResolvedPixels)
{
    VulkanShader* shader = Shader();

    struct MaskCase
    {
        bool     useDefault;
        uint64_t mask;
        uint32_t red;
    };

    const MaskCase cases[] = {{true, 0, 255}, {false, 1, 64}, {false, 3, 128}, {false, 0, 0}};

    for (const MaskCase& testCase : cases)
    {
        SCOPED_TRACE(testing::Message() << "default=" << testCase.useDefault << " mask=" << testCase.mask);

        RHITextureCreateInfo textureInfo{};

        textureInfo.type   = RHITextureType::e2D;

        textureInfo.format = DataFormat::eR8G8B8A8UNORM;

        textureInfo.width = textureInfo.height = 4;

        textureInfo.samples                    = SampleCount::e4;

        textureInfo.usageFlags.SetFlags(RHITextureUsageFlagBits::eColorAttachment, RHITextureUsageFlagBits::eTransferSrc,
                                        RHITextureUsageFlagBits::eTransferDst);

        VulkanTexture* color = static_cast<VulkanTexture*>(session->rhi.CreateTexture(textureInfo));

        textures.push_back(color);

        textureInfo.samples     = SampleCount::e1;

        VulkanTexture* resolved = static_cast<VulkanTexture*>(session->rhi.CreateTexture(textureInfo));

        textures.push_back(resolved);

        VulkanBuffer* buffer = Buffer();

        RHIRenderingLayout layout{};

        layout.SetRenderArea(0, 0, 4, 4);

        layout.AddColorRenderTarget(color->GetFormat(), color, RHIRenderTargetLoadOp::eClear, RHIRenderTargetStoreOp::eStore,
                                    RHIRenderTargetClearValue(Color(0, 0, 0, 0)));

        RHIGfxPipelineStates states{};

        states.colorBlendState.AddAttachment();

        states.multiSampleState.sampleCount = SampleCount::e4;

        if (!testCase.useDefault)
        {
            states.multiSampleState.sampleMasks = testCase.mask;
        }

        RHIPipeline* pipeline = Graphics(shader, layout, states);

        Transition(color, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0,
                   VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                   VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);

        context->RHIBeginRendering(&layout);

        context->RHIBindPipeline(pipeline);

        context->RHIDraw(3, 1, 0, 0);

        context->RHIEndRendering();

        Transition(color, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                   VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

        Transition(resolved, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                   VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

        context->RHIResolveTexture(color, resolved, 0, 0, 0, 0);

        Transition(resolved, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                   VK_PIPELINE_STAGE_TRANSFER_BIT);

        Copy(resolved, buffer, VK_IMAGE_ASPECT_COLOR_BIT);

        SubmitAndWait(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);

        const uint8_t* pixels = buffer->Map();

        for (uint32_t pixel = 0; pixel < 16; ++pixel)
        {
            SCOPED_TRACE(pixel);

            EXPECT_NEAR(pixels[pixel * 4], testCase.red, 1);

            EXPECT_EQ(pixels[pixel * 4 + 1], 0);

            EXPECT_EQ(pixels[pixel * 4 + 2], 0);
        }

        buffer->Unmap();
    }
}

TEST_F(VulkanPipelineIntegrationTest, SparseBlendMaskPreservesRenderTargetLocations)
{
    RHIRenderingLayout layout{};

    layout.SetRenderArea(0, 0, 4, 4);

    layout.numColorRenderTargets = 3;

    for (uint32_t i = 0; i < 3; ++i)
    {
        layout.colorRenderTargets[i].format = DataFormat::eR8G8B8A8UNORM;
    }

    RHIGfxPipelineStates states{};

    states.colorBlendState.attachmentsMask.set(2);

    states.colorBlendState.attachments[2].colorWriteMask.SetFlag(RHIColorComponent::eRed);

    PipelineObserver::inspect = [](const VkGraphicsPipelineCreateInfo& ci) {
        ASSERT_EQ(ci.pColorBlendState->attachmentCount, 3u);

        EXPECT_EQ(ci.pColorBlendState->pAttachments[0].colorWriteMask, 0u);

        EXPECT_EQ(ci.pColorBlendState->pAttachments[1].colorWriteMask, 0u);

        EXPECT_EQ(ci.pColorBlendState->pAttachments[2].colorWriteMask, VK_COLOR_COMPONENT_R_BIT);
    };

    Graphics(Shader(), layout, states);
}

TEST_F(VulkanPipelineIntegrationTest, SampledDepthTextureRetainsDepthAspectWithoutAttachmentUsage)
{
    VulkanTexture* texture = Texture(DataFormat::eD32SFloat, RHITextureUsageFlagBits::eSampled, false);

    EXPECT_EQ(texture->GetVkSubresourceRange().aspectMask, VK_IMAGE_ASPECT_DEPTH_BIT);

    EXPECT_EQ(texture->GetVkImageUsage(), VK_IMAGE_USAGE_SAMPLED_BIT);
}

struct DepthStencilCase
{
    DataFormat         format;
    VkFormat           vkFormat;
    VkImageAspectFlags aspects;
    const char*        name;
};

class VulkanDepthStencilIntegrationTest :
    public VulkanPipelineIntegrationTest,
    public testing::WithParamInterface<DepthStencilCase>
{};

TEST_P(VulkanDepthStencilIntegrationTest, PipelineFormatsAndRenderingClearsMatchAspects)
{
    const DepthStencilCase testCase = GetParam();

    VkImageFormatProperties properties{};

    const VkResult supported = vkGetPhysicalDeviceImageFormatProperties(
        session->rhi.GetPhysicalDevice(), testCase.vkFormat, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
        VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, 0, &properties);

    if (supported == VK_ERROR_FORMAT_NOT_SUPPORTED)
    {
        GTEST_SKIP() << testCase.name;
    }

    ASSERT_EQ(supported, VK_SUCCESS);

    const bool depth       = (testCase.aspects & VK_IMAGE_ASPECT_DEPTH_BIT) != 0;

    const bool stencil     = (testCase.aspects & VK_IMAGE_ASPECT_STENCIL_BIT) != 0;

    VulkanTexture* texture = Texture(testCase.format, RHITextureUsageFlagBits::eDepthStencilAttachment);

    EXPECT_EQ(texture->GetVkSubresourceRange().aspectMask, testCase.aspects);

    VulkanBuffer* buffer = Buffer();

    RHIRenderingLayout layout{};

    layout.SetRenderArea(0, 0, 4, 4);

    layout.AddDepthStencilRenderTarget(testCase.format, texture, RHIRenderTargetLoadOp::eClear, RHIRenderTargetStoreOp::eStore,
                                       RHIRenderTargetClearValue(0.6f, 7));

    PipelineObserver::inspect = [&](const VkGraphicsPipelineCreateInfo& ci) {
        const VkPipelineRenderingCreateInfo* rendering = static_cast<const VkPipelineRenderingCreateInfo*>(ci.pNext);

        ASSERT_NE(rendering, nullptr);

        ASSERT_EQ(rendering->sType, VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO);

        EXPECT_EQ(rendering->depthAttachmentFormat, depth ? testCase.vkFormat : VK_FORMAT_UNDEFINED);

        EXPECT_EQ(rendering->stencilAttachmentFormat, stencil ? testCase.vkFormat : VK_FORMAT_UNDEFINED);
    };

    Graphics(Shader(false, true), layout);

    Transition(texture, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, 0,
               VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
               VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT);

    context->RHIBeginRendering(&layout);

    context->RHIEndRendering();

    Transition(texture, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
               VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
               VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
               VK_PIPELINE_STAGE_TRANSFER_BIT);

    if (depth)
    {
        Copy(texture, buffer, VK_IMAGE_ASPECT_DEPTH_BIT);
    }

    if (stencil)
    {
        Copy(texture, buffer, VK_IMAGE_ASPECT_STENCIL_BIT, 64);
    }

    SubmitAndWait(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);

    const uint8_t* pixels = buffer->Map();

    for (uint32_t i = 0; i < 16; ++i)
    {
        SCOPED_TRACE(i);

        if (stencil)
        {
            EXPECT_EQ(pixels[64 + i], 7);
        }

        if (depth)
        {
            if (testCase.format == DataFormat::eD16UNORM)
            {
                EXPECT_NEAR(reinterpret_cast<const uint16_t*>(pixels)[i], 0.6 * 65535, 1);
            }
            else if (testCase.format == DataFormat::eD24UNORMS8UInt)
            {
                EXPECT_NEAR(reinterpret_cast<const uint32_t*>(pixels)[i] & 0xFFFFFFu, 0.6 * 0xFFFFFF, 2);
            }
            else
            {
                EXPECT_NEAR(reinterpret_cast<const float*>(pixels)[i], 0.6f, 1e-6f);
            }
        }
    }

    buffer->Unmap();
}

INSTANTIATE_TEST_SUITE_P(
    Formats,
    VulkanDepthStencilIntegrationTest,
    testing::Values(DepthStencilCase{DataFormat::eD16UNORM, VK_FORMAT_D16_UNORM, VK_IMAGE_ASPECT_DEPTH_BIT, "D16"},
                    DepthStencilCase{DataFormat::eD32SFloat, VK_FORMAT_D32_SFLOAT, VK_IMAGE_ASPECT_DEPTH_BIT, "D32"},
                    DepthStencilCase{DataFormat::eS8UInt, VK_FORMAT_S8_UINT, VK_IMAGE_ASPECT_STENCIL_BIT, "S8"},
                    DepthStencilCase{DataFormat::eD24UNORMS8UInt, VK_FORMAT_D24_UNORM_S8_UINT,
                                     VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, "D24S8"},
                    DepthStencilCase{DataFormat::eD32SFloatS8UInt, VK_FORMAT_D32_SFLOAT_S8_UINT,
                                     VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, "D32S8"}),
    [](const testing::TestParamInfo<DepthStencilCase>& info) { return info.param.name; });
} // namespace
