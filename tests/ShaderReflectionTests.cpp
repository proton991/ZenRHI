#include "Core/ShaderReflection.h"
#include <fstream>
#include <limits>
#include <gtest/gtest.h>

using namespace zen;

namespace
{
constexpr uint32_t                 kNoDeviceSetLimit = std::numeric_limits<uint32_t>::max();
const RHIShaderResourceDescriptor* FindBinding(const RHIShaderGroupInfo& info, NameID name)
{
    const RHIShaderResourceDescriptor* pResult = nullptr;

    for (std::vector<zen::RHIShaderResourceDescriptor> const& set : info.SRDTable)
    {
        for (zen::RHIShaderResourceDescriptor const& binding : set)
        {
            if (binding.name == std::string_view(name))
            {
                pResult = &binding;

                break;
            }
        }

        if (pResult != nullptr)
        {
            break;
        }
    }

    return pResult;
}

} // namespace

namespace
{
HeapVector<uint8_t> LoadReflectionFixture(const char* name)
{
    HeapVector<uint8_t> bytes;

    std::ifstream file(std::string(ZENRHI_TEST_SHADER_DIR) + name, std::ios::binary | std::ios::ate);

    EXPECT_TRUE(file.is_open());

    if (file.is_open())
    {
        const std::ifstream::pos_type size = file.tellg();

        EXPECT_GT(size, 0);

        if (size > 0)
        {
            bytes.resize(static_cast<size_t>(size));

            file.seekg(0);

            file.read(reinterpret_cast<char*>(bytes.data()), size);

            EXPECT_TRUE(file.good());
        }
    }

    return bytes;
}

void ExpectAccess(const RHIShaderGroupInfo& info, NameID name, bool readable, bool writable)
{
    SCOPED_TRACE(name.CStr());

    const RHIShaderResourceDescriptor* binding = FindBinding(info, name);

    ASSERT_NE(binding, nullptr);

    EXPECT_EQ(binding->readable, readable);

    EXPECT_EQ(binding->writable, writable);
}
} // namespace

TEST(ShaderReflectionTests, RayTracingStageMustMatchTheModuleExecutionModel)
{
    RHIShaderGroupSPIRVPtr spirv = MakeRefCountPtr<RHIShaderGroupSPIRV>();
    spirv->SetStageSPIRV(RHIShaderStage::eMiss, LoadReflectionFixture("rt_shared.rgen.spv"));
    RHIShaderGroupInfo output{};
    EXPECT_FALSE(RHIShaderUtil::ReflectShaderGroupInfo(spirv, kNoDeviceSetLimit, output));
    EXPECT_TRUE(output.SRDTable.empty());
}

TEST(ShaderReflectionTests, RayTracingDescriptorsMergeEveryStageAndMultipleMissModules)
{
    RHIShaderGroupSPIRVPtr spirv    = MakeRefCountPtr<RHIShaderGroupSPIRV>();
    const RHIShaderStage   stages[] = {RHIShaderStage::eRayGeneration, RHIShaderStage::eMiss, RHIShaderStage::eClosestHit,
                                       RHIShaderStage::eAnyHit, RHIShaderStage::eCallable};
    const char*            paths[] = {"rt_shared.rgen.spv", "rt_shared.rmiss.spv", "rt_shared.rchit.spv", "rt_shared.rahit.spv",
                                      "rt_shared.rcall.spv"};
    RHIFlags<RHIShaderStageFlagBits> allStages;
    for (uint32_t index = 0; index < std::size(stages); ++index)
    {
        spirv->AppendStageSPIRV(stages[index], LoadReflectionFixture(paths[index]));
        allStages.SetFlag(RHIShaderStageToFlagBits(stages[index]));
    }
    spirv->AppendStageSPIRV(RHIShaderStage::eMiss, LoadReflectionFixture("rt_secondary.rmiss.spv"));
    RHIShaderGroupInfo info;
    ASSERT_TRUE(RHIShaderUtil::ReflectShaderGroupInfo(spirv, kNoDeviceSetLimit, info));
    const RHIShaderResourceDescriptor* shared    = FindBinding(info, "RTShared");
    const RHIShaderResourceDescriptor* output    = FindBinding(info, "RTOutput");
    const RHIShaderResourceDescriptor* secondary = FindBinding(info, "RTSecondary");
    ASSERT_NE(shared, nullptr);
    ASSERT_NE(output, nullptr);
    ASSERT_NE(secondary, nullptr);
    EXPECT_EQ(shared->stageFlags, allStages);
    EXPECT_EQ(output->stageFlags, RHIFlags<RHIShaderStageFlagBits>(RHIShaderStageFlagBits::eRayGeneration));
    EXPECT_EQ(secondary->stageFlags, RHIFlags<RHIShaderStageFlagBits>(RHIShaderStageFlagBits::eMiss));
    EXPECT_EQ(secondary->binding, 2u);
    EXPECT_TRUE(shared->readable);
    EXPECT_FALSE(shared->writable);
    EXPECT_TRUE(output->writable);
}

TEST(ShaderReflectionTests, SharedSpecializationIdsMergeStagesAndKeepNewConstants)
{
    RefCountPtr<RHIShaderGroupSPIRV> spirv = MakeRefCountPtr<RHIShaderGroupSPIRV>();

    spirv->SetStageSPIRV(RHIShaderStage::eVertex, LoadReflectionFixture("pipeline.vert.spv"));

    spirv->SetStageSPIRV(RHIShaderStage::eFragment, LoadReflectionFixture("pipeline.frag.spv"));

    RHIShaderGroupInfo info{};

    EXPECT_TRUE(RHIShaderUtil::ReflectShaderGroupInfo(spirv, kNoDeviceSetLimit, info));

    ASSERT_EQ(info.specializationConstants.size(), 3u);

    std::ranges::sort(info.specializationConstants, {}, &RHIShaderSpecializationConstant::constantId);

    for (uint32_t id = 0; id < 3; ++id)
    {
        const RHIShaderSpecializationConstant& constant = info.specializationConstants[id];

        EXPECT_EQ(constant.constantId, id);

        EXPECT_TRUE(constant.stages.HasFlag(RHIShaderStageFlagBits::eFragment));

        EXPECT_EQ(constant.stages.HasFlag(RHIShaderStageFlagBits::eVertex), id < 2);
    }

    EXPECT_EQ(info.specializationConstants[0].type, RHIShaderSpecializationConstantType::eBool);

    EXPECT_TRUE(info.specializationConstants[0].bits);

    EXPECT_EQ(info.specializationConstants[1].type, RHIShaderSpecializationConstantType::eInt);

    EXPECT_EQ(info.specializationConstants[1].bits, static_cast<uint32_t>(-7));

    EXPECT_EQ(info.specializationConstants[2].type, RHIShaderSpecializationConstantType::eFloat);

    EXPECT_FLOAT_EQ(std::bit_cast<float>(info.specializationConstants[2].bits), 1.25f);
}

TEST(ShaderReflectionTests, StorageQualifiersAndMemberAccessAreReflectedFromSPIRV)
{
    HeapVector<uint8_t> bytes = LoadReflectionFixture("rdg_access.comp.spv");

    ASSERT_FALSE(bytes.empty());

    RefCountPtr<zen::RHIShaderGroupSPIRV> spirv = MakeRefCountPtr<RHIShaderGroupSPIRV>();

    spirv->SetStageSPIRV(RHIShaderStage::eCompute, std::move(bytes));

    RHIShaderGroupInfo info;

    EXPECT_TRUE(RHIShaderUtil::ReflectShaderGroupInfo(spirv, kNoDeviceSetLimit, info));

    ExpectAccess(info, "ReadBlock", true, false);

    ExpectAccess(info, "WriteBlock", false, true);

    ExpectAccess(info, "MixedBlock", true, true);

    ExpectAccess(info, "UnqualifiedBlock", true, true);

    ExpectAccess(info, "readImage", true, false);

    ExpectAccess(info, "writeImage", false, true);

    ExpectAccess(info, "readWriteImage", true, true);

    for (NameID name : {"BufferArray", "UniformArray"})
    {
        SCOPED_TRACE(name.CStr());

        const RHIShaderResourceDescriptor* binding = FindBinding(info, name);

        ASSERT_NE(binding, nullptr);

        EXPECT_EQ(binding->arraySize, 2u);

        if (binding->type == RHIShaderResourceType::eUniformBuffer)
        {
            EXPECT_GE(binding->blockSize, 4u);
        }
        else
        {
            EXPECT_EQ(binding->blockSize,
                      0u); // SPIRV-Reflect reports storage blocks as runtime-sized.
        }

        ExpectAccess(info, name, true, false);
    }
}

TEST(ShaderReflectionTests, SharedBindingAccessIsUnionedInEitherStageMergeOrder)
{
    HeapVector<uint8_t> vertex   = LoadReflectionFixture("rdg_shared.vert.spv");

    HeapVector<uint8_t> fragment = LoadReflectionFixture("rdg_shared.frag.spv");

    ASSERT_FALSE(vertex.empty());

    ASSERT_FALSE(fragment.empty());

    RHIShaderResourceDescriptor descriptors[2];

    for (uint32_t i = 0; i < 2; ++i)
    {
        RefCountPtr<zen::RHIShaderGroupSPIRV> spirv = MakeRefCountPtr<RHIShaderGroupSPIRV>();

        spirv->SetStageSPIRV(i == 0 ? RHIShaderStage::eVertex : RHIShaderStage::eFragment,
                             i == 0 ? HeapVector<uint8_t>(vertex) : HeapVector<uint8_t>(fragment));

        RHIShaderGroupInfo info;

        EXPECT_TRUE(RHIShaderUtil::ReflectShaderGroupInfo(spirv, kNoDeviceSetLimit, info));

        ASSERT_EQ(info.SRDTable.size(), 1u);

        ASSERT_EQ(info.SRDTable[0].size(), 1u);

        descriptors[i] = info.SRDTable[0][0];

        EXPECT_EQ(descriptors[i].readable, i == 0);

        EXPECT_EQ(descriptors[i].writable, i == 1);
    }

    for (bool reverse : {false, true})
    {
        RHIShaderGroupInfo info;

        for (uint32_t n = 0; n < 2; ++n)
        {
            const uint32_t i = reverse ? 1 - n : n;

            MergeOrAddSRDs(i == 0 ? RHIShaderStage::eVertex : RHIShaderStage::eFragment, descriptors[i], kNoDeviceSetLimit,
                           info);
        }

        ASSERT_EQ(info.SRDTable.size(), 1u);

        ASSERT_EQ(info.SRDTable[0].size(), 1u);

        zen::RHIShaderResourceDescriptor const& binding = info.SRDTable[0][0];

        EXPECT_TRUE(binding.readable);

        EXPECT_TRUE(binding.writable);

        EXPECT_TRUE(binding.stageFlags.HasFlag(RHIShaderStageFlagBits::eVertex));

        EXPECT_TRUE(binding.stageFlags.HasFlag(RHIShaderStageFlagBits::eFragment));
    }

    RefCountPtr<zen::RHIShaderGroupSPIRV> both = MakeRefCountPtr<RHIShaderGroupSPIRV>();

    both->SetStageSPIRV(RHIShaderStage::eVertex, std::move(vertex));

    both->SetStageSPIRV(RHIShaderStage::eFragment, std::move(fragment));

    RHIShaderGroupInfo info;

    EXPECT_TRUE(RHIShaderUtil::ReflectShaderGroupInfo(both, kNoDeviceSetLimit, info));

    ExpectAccess(info, "SharedBuffer", true, true);
}

TEST(ShaderReflectionTests, NestedAndIncompleteBlockMetadataCannotHideAccess)
{
    SpvReflectBlockVariable leaves[2]{};

    leaves[0].decoration_flags = SPV_REFLECT_DECORATION_NON_WRITABLE;

    leaves[1].decoration_flags = SPV_REFLECT_DECORATION_NON_READABLE;

    SpvReflectBlockVariable nested{};

    nested.member_count = 2;

    nested.members      = leaves;

    SpvReflectDescriptorBinding binding{};

    binding.descriptor_type        = SPV_REFLECT_DESCRIPTOR_TYPE_STORAGE_BUFFER;

    binding.name                   = "nested";

    binding.block.member_count     = 1;

    binding.block.members          = &nested;

    binding.block.decoration_flags = SPV_REFLECT_DECORATION_NON_WRITABLE;

    RHIShaderResourceDescriptor result;

    ParseSpvReflectDescriptorBinding(binding, result);

    EXPECT_TRUE(result.readable);

    EXPECT_TRUE(result.writable);

    nested.decoration_flags = SPV_REFLECT_DECORATION_NON_READABLE;

    ParseSpvReflectDescriptorBinding(binding, result);

    EXPECT_FALSE(result.readable);

    EXPECT_TRUE(result.writable);

    binding.block.members = nullptr;

    ParseSpvReflectDescriptorBinding(binding, result);

    EXPECT_TRUE(result.readable);

    EXPECT_TRUE(result.writable);
}

TEST(ShaderReflectionTests, MultiLocationVertexInputCannotPublishASingleAttribute)
{
    RHIShaderGroupSPIRVPtr spirv = MakeRefCountPtr<RHIShaderGroupSPIRV>();
    for (const char* shader : {"review_array.vert.spv", "review_matrix.vert.spv"})
    {
        SCOPED_TRACE(shader);
        spirv->SetStageSPIRV(RHIShaderStage::eVertex, LoadReflectionFixture(shader));
        RHIShaderGroupInfo info;
        EXPECT_FALSE(RHIShaderUtil::ReflectShaderGroupInfo(spirv, kNoDeviceSetLimit, info));
        EXPECT_EQ(info.vertexBindingStride, 0u);
    }
}

TEST(ShaderReflectionTests, CorruptInputDoesNotPublishPartialReflection)
{
    RHIShaderGroupSPIRVPtr spirv = MakeRefCountPtr<RHIShaderGroupSPIRV>();

    spirv->SetStageSPIRV(RHIShaderStage::eCompute, HeapVector<uint8_t>{0, 1, 2, 3});

    RHIShaderGroupInfo info;

    info.vertexBindingStride = 123;

    EXPECT_FALSE(RHIShaderUtil::ReflectShaderGroupInfo(spirv, kNoDeviceSetLimit, info));

    EXPECT_EQ(info.vertexBindingStride, 123u);
}

TEST(ShaderReflectionTests, UnsupportedMetadataFailsBeforePublication)
{
    RHIShaderGroupInfo info;

    SpvReflectShaderModule module{};

    module.push_constant_block_count = 2;

    EXPECT_FALSE(ParseSpvPushConstants(RHIShaderStage::eCompute, &module, info));

    SpvReflectTypeDescription type{};

    type.op                          = SpvOpTypeFloat;

    type.traits.numeric.scalar.width = 64;

    double value                     = 1.0;

    SpvReflectSpecializationConstant constant{};

    constant.type_description   = &type;

    constant.default_value      = &value;

    constant.default_value_size = sizeof(value);

    module.spec_constant_count  = 1;

    module.spec_constants       = &constant;

    EXPECT_FALSE(ParseSpvSpecializationConstant(RHIShaderStage::eCompute, &module, info));

    EXPECT_TRUE(info.specializationConstants.empty());

    for (SpvReflectDescriptorType unsupported :
         {SPV_REFLECT_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, SPV_REFLECT_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC})
    {
        SpvReflectDescriptorBinding binding{};

        binding.descriptor_type = unsupported;

        RHIShaderResourceDescriptor descriptor{};

        EXPECT_FALSE(ParseSpvReflectDescriptorBinding(binding, descriptor));
    }
}

TEST(ShaderReflectionTests, AccelerationStructureUsesAnOrdinaryReadOnlyDescriptor)
{
    SpvReflectDescriptorBinding binding{};
    binding.descriptor_type = SPV_REFLECT_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    binding.set             = 6;
    binding.binding         = 2;
    RHIShaderResourceDescriptor descriptor{};
    ASSERT_TRUE(ParseSpvReflectDescriptorBinding(binding, descriptor));
    EXPECT_EQ(descriptor.type, RHIShaderResourceType::eAccelerationStructure);
    EXPECT_EQ(descriptor.set, 6u);
    EXPECT_EQ(descriptor.binding, 2u);
    EXPECT_EQ(descriptor.arraySize, 1u);
    EXPECT_TRUE(descriptor.readable);
    EXPECT_FALSE(descriptor.writable);
    EXPECT_FALSE(descriptor.bindless);
}

TEST(ShaderReflectionTests, GlobalTextureHeapsRequireTheirDeclaredImageShape)
{
    struct HeapShape
    {
        uint32_t binding;
        SpvDim   dimension;
        uint32_t arrayed;
    };
    const HeapShape heaps[] = {{ZEN_BINDLESS_HEAP_BINDING_TEXTURE2D, SpvDim2D, 0},
                               {ZEN_BINDLESS_HEAP_BINDING_TEXTURECUBE, SpvDimCube, 0},
                               {ZEN_BINDLESS_HEAP_BINDING_TEXTURE2DARRAY, SpvDim2D, 1},
                               {ZEN_BINDLESS_HEAP_BINDING_TEXTURECUBEARRAY, SpvDimCube, 1}};
    for (const HeapShape& heap : heaps)
    {
        for (SpvDim dimension : {SpvDim1D, SpvDim2D, SpvDim3D, SpvDimCube, SpvDimRect, SpvDimBuffer, SpvDimSubpassData})
        {
            for (uint32_t arrayed : {0u, 1u})
            {
                for (uint32_t multisampled : {0u, 1u})
                {
                    SCOPED_TRACE(::testing::Message() << "binding=" << heap.binding << " dim=" << dimension
                                                      << " arrayed=" << arrayed << " ms=" << multisampled);
                    SpvReflectDescriptorBinding binding{};
                    binding.descriptor_type  = SPV_REFLECT_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
                    binding.set              = kGlobalBindlessHeapIndex;
                    binding.binding          = heap.binding;
                    binding.array.dims_count = 1;
                    binding.array.dims[0]    = SPV_REFLECT_ARRAY_DIM_RUNTIME;
                    binding.image.dim        = dimension;
                    binding.image.arrayed    = arrayed;
                    binding.image.ms         = multisampled;
                    RHIShaderResourceDescriptor descriptor{};
                    const bool expected = dimension == heap.dimension && arrayed == heap.arrayed && multisampled == 0;
                    EXPECT_EQ(ParseSpvReflectDescriptorBinding(binding, descriptor), expected);
                    EXPECT_TRUE(descriptor.bindless);
                }
            }
        }
    }
}

TEST(ShaderReflectionTests, OrdinaryTextureArraysAndGlobalSamplersKeepTheirReflectionContract)
{
    SpvReflectDescriptorBinding binding{};
    binding.descriptor_type  = SPV_REFLECT_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    binding.set              = kGlobalBindlessHeapIndex + 1;
    binding.binding          = ZEN_BINDLESS_HEAP_BINDING_TEXTURE2D;
    binding.array.dims_count = 1;
    binding.array.dims[0]    = 3;
    binding.image.dim        = SpvDim2D;
    binding.image.arrayed    = 1;
    RHIShaderResourceDescriptor descriptor{};
    ASSERT_TRUE(ParseSpvReflectDescriptorBinding(binding, descriptor));
    EXPECT_EQ(descriptor.type, RHIShaderResourceType::eTexture);
    EXPECT_EQ(descriptor.arraySize, 3u);
    EXPECT_FALSE(descriptor.bindless);

    binding                  = {};
    binding.descriptor_type  = SPV_REFLECT_DESCRIPTOR_TYPE_SAMPLER;
    binding.set              = kGlobalBindlessHeapIndex;
    binding.binding          = ZEN_BINDLESS_HEAP_BINDING_SAMPLER;
    binding.array.dims_count = 1;
    binding.array.dims[0]    = SPV_REFLECT_ARRAY_DIM_RUNTIME;
    descriptor               = {};
    ASSERT_TRUE(ParseSpvReflectDescriptorBinding(binding, descriptor));
    EXPECT_EQ(descriptor.type, RHIShaderResourceType::eSampler);
    EXPECT_TRUE(descriptor.bindless);
}

TEST(ShaderReflectionTests, ArrayHeapShadersReflectAndRejectMismatchedImageDeclarationsBeforePublication)
{
    for (const char* fixture : {"binding_bindless_array.comp.spv", "binding_bindless_cube_array.comp.spv"})
    {
        SCOPED_TRACE(fixture);
        HeapVector<uint8_t>    bytes = LoadReflectionFixture(fixture);
        RHIShaderGroupSPIRVPtr spirv = MakeRefCountPtr<RHIShaderGroupSPIRV>();
        spirv->SetStageSPIRV(RHIShaderStage::eCompute, HeapVector<uint8_t>(bytes));
        RHIShaderGroupInfo info{};
        ASSERT_TRUE(RHIShaderUtil::ReflectShaderGroupInfo(spirv, kNoDeviceSetLimit, info));
        EXPECT_TRUE(RHIShaderUtil::ValidateShaderLayout(info, kNoDeviceSetLimit));

        // Remove the array shape from OpTypeImage without changing its descriptor binding.
        // The old heap declarations stay valid; the new array heap must reject the mismatch.
        HeapVector<uint32_t> words(bytes.size() / sizeof(uint32_t));
        std::memcpy(words.data(), bytes.data(), bytes.size());
        uint32_t changed = 0;
        for (size_t offset = 5; offset < words.size(); offset += words[offset] >> 16)
        {
            if ((words[offset] & 0xffffu) == SpvOpTypeImage)
            {
                ASSERT_GE(words[offset] >> 16, 9u);
                if (words[offset + 5] == 1)
                {
                    words[offset + 5] = 0;
                    ++changed;
                }
            }
        }
        ASSERT_GT(changed, 0u);
        std::memcpy(bytes.data(), words.data(), bytes.size());
        spirv->SetStageSPIRV(RHIShaderStage::eCompute, std::move(bytes));
        RHIShaderGroupInfo rejected{};
        rejected.vertexBindingStride = 123;
        EXPECT_FALSE(RHIShaderUtil::ReflectShaderGroupInfo(spirv, kNoDeviceSetLimit, rejected));
        EXPECT_TRUE(rejected.SRDTable.empty());
        EXPECT_EQ(rejected.vertexBindingStride, 123u);
    }
}

TEST(ShaderReflectionTests, ReflectionRejectsSetsAtOrBeyondTheDeviceLimit)
{
    RHIShaderResourceDescriptor descriptor{};
    descriptor.set  = 5;
    descriptor.type = RHIShaderResourceType::eStorageBuffer;
    RHIShaderGroupInfo rejected{};
    EXPECT_FALSE(MergeOrAddSRDs(RHIShaderStage::eCompute, descriptor, 5, rejected));
    // A rejected index never sizes the table, so a corrupt index cannot grow it.
    EXPECT_TRUE(rejected.SRDTable.empty());
    RHIShaderGroupInfo accepted{};
    EXPECT_TRUE(MergeOrAddSRDs(RHIShaderStage::eCompute, descriptor, 6, accepted));
    EXPECT_EQ(accepted.SRDTable.size(), 6u);
}

TEST(ShaderReflectionTests, LayoutRulesRejectRuntimeArraysReservedSetAndDuplicateNames)
{
    // Vulkan's guaranteed minimum, so these layouts are valid on every device.
    const uint32_t     sets = RHIGPUInfo{}.maxBoundDescriptorSets;
    RHIShaderGroupInfo info{};
    info.SRDTable.resize(2);
    RHIShaderResourceDescriptor binding{};
    binding.set     = 1;
    binding.binding = 0;
    binding.name    = "Block";
    binding.type    = RHIShaderResourceType::eUniformBuffer;
    info.SRDTable[1].push_back(binding);
    EXPECT_TRUE(RHIShaderUtil::ValidateShaderLayout(info, sets));
    info.SRDTable[1][0].bindless = true;
    EXPECT_FALSE(RHIShaderUtil::ValidateShaderLayout(info, sets));
    info.SRDTable[1][0].bindless = false;
    binding.binding              = 1;
    info.SRDTable[1].push_back(binding);
    EXPECT_FALSE(RHIShaderUtil::ValidateShaderLayout(info, sets));
    info.SRDTable[1].clear();
    binding.set = 0;
    info.SRDTable[0].push_back(binding);
    EXPECT_FALSE(RHIShaderUtil::ValidateShaderLayout(info, sets));
}

TEST(ShaderReflectionTests, LayoutRulesFollowTheDeviceDescriptorSetLimit)
{
    RHIShaderGroupInfo info{};
    info.SRDTable.resize(10);
    RHIShaderResourceDescriptor binding{};
    binding.set  = 9;
    binding.name = "HighSet";
    binding.type = RHIShaderResourceType::eStorageBuffer;
    info.SRDTable[9].push_back(binding);
    // Ten sets fit a device that binds ten, and exceed one that binds nine; no engine count applies.
    EXPECT_TRUE(RHIShaderUtil::ValidateShaderLayout(info, 10));
    EXPECT_FALSE(RHIShaderUtil::ValidateShaderLayout(info, 9));
}
