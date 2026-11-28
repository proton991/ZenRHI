#pragma once
#include "ZenRHI/RHIDiagnostics.h"
#include "ZenRHI/RHIShaderUtil.h"
#include <bit>
#include <cstring>
#include <algorithm>
#include "spirv_reflect.h"
namespace zen
{
inline constexpr SpvExecutionModel kShaderExecutionModels[] = {SpvExecutionModelVertex,
                                                               SpvExecutionModelTessellationControl,
                                                               SpvExecutionModelTessellationEvaluation,
                                                               SpvExecutionModelGeometry,
                                                               SpvExecutionModelFragment,
                                                               SpvExecutionModelGLCompute,
                                                               SpvExecutionModelRayGenerationKHR,
                                                               SpvExecutionModelMissKHR,
                                                               SpvExecutionModelClosestHitKHR,
                                                               SpvExecutionModelAnyHitKHR,
                                                               SpvExecutionModelCallableKHR};
static_assert(std::size(kShaderExecutionModels) == ToUnderlying(RHIShaderStage::eMax));

inline bool ParseSpvVertexInput(const SpvReflectShaderModule* module, RHIShaderGroupInfo& info)
{
    uint32_t count = 0;

    bool valid     = spvReflectEnumerateInputVariables(module, &count, nullptr) == SPV_REFLECT_RESULT_SUCCESS;

    HeapVector<SpvReflectInterfaceVariable*> inputs(count);

    valid = valid && spvReflectEnumerateInputVariables(module, &count, inputs.data()) == SPV_REFLECT_RESULT_SUCCESS;

    if (valid)
    {
        std::sort(inputs.begin(), inputs.end(),
                  [](const SpvReflectInterfaceVariable* lhs, const SpvReflectInterfaceVariable* rhs) {
                      return lhs->location < rhs->location;
                  });

        for (const SpvReflectInterfaceVariable* input : inputs)
        {
            if ((input->decoration_flags & SPV_REFLECT_DECORATION_BUILT_IN) == 0)
            {
                valid = input->format != SPV_REFLECT_FORMAT_UNDEFINED && input->numeric.scalar.width != 0
                     && input->array.dims_count == 0 && input->numeric.matrix.column_count == 0;

                if (!valid)
                {
                    break;
                }

                RHIShaderGroupInfo::VertexInputAttribute attribute{};

                attribute.name     = input->name == nullptr ? "" : input->name;

                attribute.location = input->location;

                attribute.binding  = 0;

                attribute.offset   = info.vertexBindingStride;

                attribute.format   = static_cast<DataFormat>(input->format);

                info.vertexBindingStride +=
                    input->numeric.scalar.width / 8 * std::max(1u, input->numeric.vector.component_count);

                info.vertexInputAttributes.push_back(attribute);
            }
        }
    }

    return valid;
}

inline bool ParseSpvPushConstants(RHIShaderStage stage, const SpvReflectShaderModule* module, RHIShaderGroupInfo& info)
{
    uint32_t count = 0;

    bool valid     = spvReflectEnumeratePushConstantBlocks(module, &count, nullptr) == SPV_REFLECT_RESULT_SUCCESS && count <= 1;

    if (valid && count == 1)
    {
        SpvReflectBlockVariable* block = nullptr;

        valid = spvReflectEnumeratePushConstantBlocks(module, &count, &block) == SPV_REFLECT_RESULT_SUCCESS && block != nullptr;

        if (valid)
        {
            valid = block->size != 0 && block->offset <= block->size && block->size % 4 == 0 && block->offset % 4 == 0;

            if (valid)
            {
                // Reflect reports the end of the block, including any initial offset.
                // A single range covers the union of the stages' declared members.
                info.pushConstants.size = std::max(info.pushConstants.size, block->size);

                info.pushConstants.stageFlags.SetFlag(RHIShaderStageToFlagBits(stage));

                info.pushConstants.name = block->type_description != nullptr && block->type_description->type_name != nullptr
                                            ? block->type_description->type_name
                                            : "";
            }
        }
    }

    if (!valid)
    {
        ZEN_RHI_LOG_ERROR("Shader stage {} requires at most one aligned push-constant block", RHIShaderStageToString(stage));
    }

    return valid;
}

inline bool ParseSpvSpecializationConstant(RHIShaderStage stage, const SpvReflectShaderModule* module, RHIShaderGroupInfo& info)
{
    uint32_t count = 0;

    bool valid     = spvReflectEnumerateSpecializationConstants(module, &count, nullptr) == SPV_REFLECT_RESULT_SUCCESS;

    HeapVector<SpvReflectSpecializationConstant*> constants(count);

    valid = valid && spvReflectEnumerateSpecializationConstants(module, &count, constants.data()) == SPV_REFLECT_RESULT_SUCCESS;

    for (uint32_t i = 0; valid && i < count; ++i)
    {
        const SpvReflectSpecializationConstant& reflected = *constants[i];

        valid = reflected.type_description != nullptr && reflected.default_value != nullptr
             && reflected.default_value_size == sizeof(uint32_t)
             && (reflected.type_description->op == SpvOpTypeBool
                 || reflected.type_description->traits.numeric.scalar.width == 32);

        if (valid)
        {
            RHIShaderSpecializationConstant value{};

            value.constantId = reflected.constant_id;

            std::memcpy(&value.bits, reflected.default_value, sizeof(value.bits));

            switch (reflected.type_description->op)
            {
                case SpvOpTypeBool:
                    value.type = RHIShaderSpecializationConstantType::eBool;
                    value.bits = value.bits != 0;
                    break;
                case SpvOpTypeInt: value.type = RHIShaderSpecializationConstantType::eInt; break;
                case SpvOpTypeFloat: value.type = RHIShaderSpecializationConstantType::eFloat; break;
                default: valid = false; break;
            }

            value.stages.SetFlag(RHIShaderStageToFlagBits(stage));

            bool existed = false;

            for (RHIShaderSpecializationConstant& previous : info.specializationConstants)
            {
                if (previous.constantId == value.constantId)
                {
                    existed = true;

                    valid   = valid && previous.type == value.type && previous.bits == value.bits;

                    previous.stages.SetFlag(RHIShaderStageToFlagBits(stage));
                }
            }

            if (valid && !existed)
            {
                info.specializationConstants.push_back(value);
            }
        }
    }

    if (!valid)
    {
        ZEN_RHI_LOG_ERROR(
            "Shader stage {} has invalid specialization constants; only compatible Boolean and 32-bit scalar constants are supported",
            RHIShaderStageToString(stage));
    }

    return valid;
}

// Member flags describe that member, including a containing struct/array. The bundled
// reflector synthesizes NonWritable on a block when ANY member is readonly, so that
// root block flag cannot classify the whole binding. Union member capabilities instead.
inline bool BlockMemberAllowsAccess(const SpvReflectBlockVariable& member, uint32_t forbiddenDecoration, uint32_t depth)
{
    const uint32_t typeFlags = member.type_description ? member.type_description->decoration_flags : 0;
    bool           allowed   = false;

    if (((member.decoration_flags | typeFlags) & forbiddenDecoration) == 0)
    {
        // Unknown/recursive metadata must not hide an access.
        allowed = member.member_count == 0 || member.members == nullptr || depth >= 32;

        for (uint32_t i = 0; !allowed && i < member.member_count; ++i)
        {
            allowed = BlockMemberAllowsAccess(member.members[i], forbiddenDecoration, depth + 1);
        }
    }

    return allowed;
}

inline bool DescriptorBindingAllowsAccess(const SpvReflectDescriptorBinding& binding, uint32_t forbiddenDecoration)
{
    const uint32_t typeFlags = binding.type_description ? binding.type_description->decoration_flags : 0;
    bool           allowed   = false;

    if (((binding.decoration_flags | typeFlags) & forbiddenDecoration) == 0)
    {
        if (binding.block.member_count == 0)
        {
            allowed = (binding.block.decoration_flags & forbiddenDecoration) == 0;
        }
        else
        {
            allowed = binding.block.members == nullptr;

            for (uint32_t i = 0; !allowed && i < binding.block.member_count; ++i)
            {
                allowed = BlockMemberAllowsAccess(binding.block.members[i], forbiddenDecoration, 0);
            }
        }
    }

    return allowed;
}

// How a reflected descriptor maps to a shader resource. Dynamic buffers have no mapping.
struct SpvDescriptorMapping
{
    SpvReflectDescriptorType spirv;
    RHIShaderResourceType    type;
    // The binding is a block with a size.
    bool block;
    // NonWritable and NonReadable decorations restrict the access; other types are read-only.
    bool storage;
};

inline constexpr SpvDescriptorMapping kSpvDescriptorMappings[] = {
    {SPV_REFLECT_DESCRIPTOR_TYPE_SAMPLER, RHIShaderResourceType::eSampler, false, false},
    {SPV_REFLECT_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, RHIShaderResourceType::eSamplerWithTexture, false, false},
    {SPV_REFLECT_DESCRIPTOR_TYPE_SAMPLED_IMAGE, RHIShaderResourceType::eTexture, false, false},
    {SPV_REFLECT_DESCRIPTOR_TYPE_STORAGE_IMAGE, RHIShaderResourceType::eImage, false, true},
    {SPV_REFLECT_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, RHIShaderResourceType::eTextureBuffer, false, false},
    {SPV_REFLECT_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, RHIShaderResourceType::eImageBuffer, false, true},
    {SPV_REFLECT_DESCRIPTOR_TYPE_UNIFORM_BUFFER, RHIShaderResourceType::eUniformBuffer, true, false},
    {SPV_REFLECT_DESCRIPTOR_TYPE_STORAGE_BUFFER, RHIShaderResourceType::eStorageBuffer, true, true},
    {SPV_REFLECT_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, RHIShaderResourceType::eInputAttachment, false, false},
    {SPV_REFLECT_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, RHIShaderResourceType::eAccelerationStructure, false, false},
};

inline bool ParseSpvReflectDescriptorBinding(const SpvReflectDescriptorBinding& reflBinding, RHIShaderResourceDescriptor& srd)
{
    bool needBlockSize = false;
    bool writable      = false;
    bool readable      = true;

    if (reflBinding.type_description != nullptr && reflBinding.type_description->type_name != nullptr)
    {
        srd.name = reflBinding.type_description->type_name;
    }
    else
    {
        srd.name = reflBinding.name != nullptr ? reflBinding.name : "";
    }

    srd.set                             = reflBinding.set;
    srd.binding                         = reflBinding.binding;
    srd.arraySize                       = 1;

    const SpvDescriptorMapping* mapping = nullptr;

    for (const SpvDescriptorMapping& candidate : kSpvDescriptorMappings)
    {
        if (candidate.spirv == reflBinding.descriptor_type)
        {
            mapping = &candidate;
        }
    }

    if (mapping != nullptr)
    {
        srd.type      = mapping->type;
        needBlockSize = mapping->block;
        if (mapping->storage)
        {
            writable = DescriptorBindingAllowsAccess(reflBinding, SPV_REFLECT_DECORATION_NON_WRITABLE);
            readable = DescriptorBindingAllowsAccess(reflBinding, SPV_REFLECT_DECORATION_NON_READABLE);
        }
    }
    else
    {
        ZEN_RHI_LOG_ERROR("Descriptor '{}' has unsupported type {}", srd.name,
                          static_cast<int32_t>(reflBinding.descriptor_type));
    }

    // Contradictory or incomplete restrictions must not suppress both capabilities.
    if (!readable && !writable)
    {
        readable = writable = true;
    }

    srd.readable = readable;
    srd.writable = writable;

    if (mapping != nullptr)
    {
        for (uint32_t i = 0; i < reflBinding.array.dims_count; i++)
        {
            const uint32_t arrayDimension = reflBinding.array.dims[i];

            if (arrayDimension == SPV_REFLECT_ARRAY_DIM_RUNTIME)
            {
                srd.bindless = true;
            }
            else
            {
                if (arrayDimension == 0 || srd.arraySize > UINT32_MAX / arrayDimension)
                {
                    srd.type = RHIShaderResourceType::eMax;
                }
                else
                {
                    srd.arraySize *= arrayDimension;
                }
            }
        }
    }

    if (needBlockSize)
    {
        srd.blockSize = reflBinding.block.size;
    }

    if (srd.bindless && srd.set == kGlobalBindlessHeapIndex && srd.type == RHIShaderResourceType::eTexture)
    {
        SpvDim   expectedDimension = SpvDimMax;
        uint32_t expectedArrayed   = 0;
        switch (srd.binding)
        {
            case ZEN_BINDLESS_HEAP_BINDING_TEXTURE2D: expectedDimension = SpvDim2D; break;
            case ZEN_BINDLESS_HEAP_BINDING_TEXTURECUBE: expectedDimension = SpvDimCube; break;
            case ZEN_BINDLESS_HEAP_BINDING_TEXTURE2DARRAY:
                expectedDimension = SpvDim2D;
                expectedArrayed   = 1;
                break;
            case ZEN_BINDLESS_HEAP_BINDING_TEXTURECUBEARRAY:
                expectedDimension = SpvDimCube;
                expectedArrayed   = 1;
                break;
            default: break;
        }
        if (reflBinding.image.dim != expectedDimension || reflBinding.image.arrayed != expectedArrayed
            || reflBinding.image.ms != 0)
        {
            ZEN_RHI_LOG_ERROR(
                "Global texture heap binding {} has an incompatible image dimension, array shape, or sample count",
                srd.binding);
            srd.type = RHIShaderResourceType::eMax;
        }
    }
    return srd.type != RHIShaderResourceType::eMax;
}

inline bool MergeOrAddSRDs(RHIShaderStage               stage,
                           RHIShaderResourceDescriptor& descriptor,
                           uint32_t                     maxDescriptorSets,
                           RHIShaderGroupInfo&          info)
{
    // The device limit also bounds the table size, so a corrupt set index cannot grow it.
    bool valid = descriptor.set < maxDescriptorSets;

    if (!valid)
    {
        ZEN_RHI_LOG_ERROR("Descriptor set {} (binding {}) is not below the device's {} descriptor sets", descriptor.set,
                          descriptor.binding, maxDescriptorSets);
    }
    else
    {
        if (info.SRDTable.size() <= descriptor.set)
        {
            info.SRDTable.resize(descriptor.set + 1);
        }

        bool existed = false;

        for (RHIShaderResourceDescriptor& previous : info.SRDTable[descriptor.set])
        {
            if (previous.binding == descriptor.binding)
            {
                existed = true;

                valid   = previous.type == descriptor.type && previous.arraySize == descriptor.arraySize
                       && previous.blockSize == descriptor.blockSize && previous.bindless == descriptor.bindless;

                previous.stageFlags.SetFlag(RHIShaderStageToFlagBits(stage));

                previous.readable |= descriptor.readable;

                previous.writable |= descriptor.writable;
            }
        }

        if (!existed)
        {
            descriptor.stageFlags.SetFlag(RHIShaderStageToFlagBits(stage));

            info.SRDTable[descriptor.set].push_back(descriptor);
        }
    }

    return valid;
}

} // namespace zen
