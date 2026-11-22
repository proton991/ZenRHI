#include "ZenRHI/RHIDiagnostics.h"
#include "ShaderReflection.h"
namespace zen
{
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

            std::span<const uint8_t> code = shader.code;

            const SpvReflectResult result = HasValidSpirvStructure(code)
                                              ? spvReflectCreateShaderModule(code.size(), code.data(), &module)
                                              : SPV_REFLECT_RESULT_ERROR_SPIRV_INVALID_CODE_SIZE;

            valid                         = result == SPV_REFLECT_RESULT_SUCCESS;

            if (valid)
            {
                static constexpr SpvExecutionModel executionModels[] = {SpvExecutionModelVertex,
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
                static_assert(std::size(executionModels) == ToUnderlying(RHIShaderStage::eMax));
                uint32_t count = 0;

                valid = stage < RHIShaderStage::eMax && module.spirv_execution_model == executionModels[ToUnderlying(stage)]
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
