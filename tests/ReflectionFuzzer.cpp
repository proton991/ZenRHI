#include "../src/Core/ShaderReflection.h"

namespace
{
// Read only complete instruction headers and the entry point model. The reflection API
// still receives the original input, including invalid modules and partial trailing words.
zen::RHIShaderStage InputStage(std::span<const uint8_t> data)
{
    zen::RHIShaderStage stage = zen::RHIShaderStage::eCompute;
    const size_t        words = data.size() / sizeof(uint32_t);
    for (size_t offset = 5; offset < words;)
    {
        uint32_t instruction;
        std::memcpy(&instruction, data.data() + offset * sizeof(uint32_t), sizeof(instruction));
        const uint32_t length = instruction >> SpvWordCountShift;
        if (!length || length > words - offset)
        {
            break;
        }
        if ((instruction & SpvOpCodeMask) == SpvOpEntryPoint && length >= 2)
        {
            uint32_t model;
            std::memcpy(&model, data.data() + (offset + 1) * sizeof(uint32_t), sizeof(model));
            const SpvExecutionModel* found =
                std::find(std::begin(zen::kShaderExecutionModels), std::end(zen::kShaderExecutionModels), model);
            if (found != std::end(zen::kShaderExecutionModels))
            {
                stage = static_cast<zen::RHIShaderStage>(found - zen::kShaderExecutionModels);
            }
            break;
        }
        offset += length;
    }
    return stage;
}

void IgnoreShaderDiagnostic(const zen::RHIMessage& diagnostic) noexcept {}
} // namespace

extern "C" int LLVMFuzzerInitialize(int*, char***)
{
    zen::SetRHIMessageCallback(IgnoreShaderDiagnostic);
    return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    zen::RHIShaderGroupSPIRVPtr    module = zen::MakeRefCountPtr<zen::RHIShaderGroupSPIRV>();
    const std::span<const uint8_t> input(data, size);
    module->AppendStageSPIRV(InputStage(input), input);
    zen::RHIShaderGroupInfo info{};
    (void)zen::RHIShaderUtil::ReflectShaderGroupInfo(module, 8, info);
    return 0;
}
