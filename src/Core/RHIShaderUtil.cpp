#include "ZenRHI/RHIDiagnostics.h"
#include "ShaderReflection.h"
#include <spirv-tools/libspirv.h>

namespace zen
{
namespace
{
SpvReflectResult ReflectValidatedModule(std::span<const uint8_t> code, SpvReflectShaderModule& module)
{
    SpvReflectResult result = SPV_REFLECT_RESULT_ERROR_SPIRV_INVALID_CODE_SIZE;
    if (code.size() >= 5 * sizeof(uint32_t) && code.size() % sizeof(uint32_t) == 0)
    {
        HeapVector<uint32_t> words(code.size() / sizeof(uint32_t));
        std::memcpy(words.data(), code.data(), code.size());
        // Validate IDs, operand types and instruction relationships before reflection follows them.
        spv_context    context = spvContextCreate(SPV_ENV_UNIVERSAL_1_6);
        spv_diagnostic diagnostic{};
        if (context && spvValidateBinary(context, words.data(), words.size(), &diagnostic) == SPV_SUCCESS)
        {
            result = spvReflectCreateShaderModule(code.size(), words.data(), &module);
        }
        else
        {
            result = SPV_REFLECT_RESULT_ERROR_PARSE_FAILED;
            if (diagnostic)
            {
                ZEN_RHI_LOG_ERROR("Invalid SPIR-V at word {}: {}", diagnostic->position.index, diagnostic->error);
            }
        }
        spvDiagnosticDestroy(diagnostic);
        spvContextDestroy(context);
    }
    return result;
}
} // namespace

bool RHIShaderUtil::ReflectShaderGroupInfo(RHIShaderGroupSPIRVPtr spirv, uint32_t maxDescriptorSets, RHIShaderGroupInfo& output)
{
    bool valid    = spirv != nullptr;

    bool hasStage = false;

    RHIShaderGroupInfo info{};

    for (uint32_t i = 0; valid && i < spirv->GetModules().size(); ++i)
    {
        const RHIShaderGroupSPIRV::Module& shader = spirv->GetModules()[i];
        const RHIShaderStage               stage  = shader.stage;

        if (spirv->HasShaderStage(stage))
        {
            hasStage = true;

            SpvReflectShaderModule module{};

            const SpvReflectResult result = ReflectValidatedModule(shader.code, module);

            valid                         = result == SPV_REFLECT_RESULT_SUCCESS;

            if (valid)
            {
                uint32_t count = 0;

                valid          = stage < RHIShaderStage::eMax
                              && module.spirv_execution_model == kShaderExecutionModels[ToUnderlying(stage)]
                              && spvReflectEnumerateDescriptorSets(&module, &count, nullptr) == SPV_REFLECT_RESULT_SUCCESS;

                HeapVector<SpvReflectDescriptorSet*> sets(count);

                valid = valid && spvReflectEnumerateDescriptorSets(&module, &count, sets.data()) == SPV_REFLECT_RESULT_SUCCESS;

                for (uint32_t set = 0; valid && set < count; ++set)
                {
                    for (uint32_t binding = 0; valid && binding < sets[set]->binding_count; ++binding)
                    {
                        RHIShaderResourceDescriptor descriptor{};

                        valid = ParseSpvReflectDescriptorBinding(*sets[set]->bindings[binding], descriptor)
                             && MergeOrAddSRDs(stage, descriptor, maxDescriptorSets, info);
                    }
                }

                valid = valid && ParseSpvSpecializationConstant(stage, &module, info)
                     && (stage != RHIShaderStage::eVertex || ParseSpvVertexInput(&module, info))
                     && ParseSpvPushConstants(stage, &module, info);

                spvReflectDestroyShaderModule(&module);
            }

            if (!valid)
            {
                ZEN_RHI_LOG_ERROR("SPIR-V reflection rejected stage {} (result {}): corrupt or unsupported shader input",
                                  RHIShaderStageToString(stage), static_cast<int32_t>(result));
            }
        }
    }

    valid = valid && hasStage;

    if (valid)
    {
        output = std::move(info);
    }

    return valid;
}
} // namespace zen
