#include "Example.h"
#include <fstream>

namespace example
{
RHICommandListPtr ComputePass::Commands(RHICommandContextType queue)
{
    IRHICommandContext* context = device.GetCommandContext(queue);
    return context ? RHICommandListPtr(RHICommandList::Create(context)) : RHICommandListPtr{};
}

void ComputePass::Dispatch(RHICommandList& commands, RHIBatchedShaderParameters& parameters)
{
    parameters.AddResourceParam(*shader.GetSRDByLocation(1, 0), &output, nullptr, 0);
    commands.BindPipeline(RHIPipelineType::eCompute, &pipeline);
    commands.SetShaderParameters(parameters);
    commands.Dispatch(1, 1, 1);
    RHIMemoryTransition readback{RHIAccessFlagBits::eShaderWrite, RHIAccessFlagBits::eHostRead};
    commands.AddTransitions(RHIPipelineStageFlagBits::eComputeShader, RHIPipelineStageFlagBits::eHost, {&readback, 1}, {}, {});
}

bool ComputePass::SubmitAndCheck(RHICommandList& commands, uint32_t expected)
{
    return Check(device.SubmitFrame(commands, nullptr).Wait(), expected);
}

bool ComputePass::Check(const RHIBatchResult& batch, uint32_t expected)
{
    device.WaitDeviceIdle();
    bool ok = batch.submission == RHISubmissionResult::eSuccess && !device.AreSubmissionsBlocked();
    if (ok)
    {
        const uint8_t* mapped = output.Map();
        uint32_t       value{};
        ok = mapped != nullptr;
        if (mapped)
        {
            std::memcpy(&value, mapped, sizeof(value));
            output.Unmap();
            std::printf("GPU result %u (expected %u)\n", value, expected);
            ok = value == expected;
        }
    }
    return ok;
}

int Execute(RHICommandListExecutor& device)
{
    std::ifstream        file(EXAMPLE_SHADER, std::ios::binary | std::ios::ate);
    const std::streamoff size = file ? static_cast<std::streamoff>(file.tellg()) : 0;
    HeapVector<uint8_t>  bytes(size > 0 ? static_cast<size_t>(size) : 0);
    file.seekg(0);
    int result = 1;
    if (size > 0 && file.read(reinterpret_cast<char*>(bytes.data()), size))
    {
        RHIShaderCreateInfo info{};
        info.spirv = MakeRefCountPtr<RHIShaderGroupSPIRV>();
        info.spirv->AppendStageSPIRV(RHIShaderStage::eCompute, {bytes.data(), bytes.size()});
        info.stageFlags.SetFlag(RHIShaderStageFlagBits::eCompute);
        RHIResourcePtr<RHIShader>   shader = RHIResourcePtr<RHIShader>::Adopt(device.CreateShader(info));
        RHIResourcePtr<RHIPipeline> pipeline;
        if (shader)
        {
            pipeline = RHIResourcePtr<RHIPipeline>::Adopt(device.CreatePipeline(RHIComputePipelineCreateInfo{shader.Get()}));
        }
        RHIBufferCreateInfo desc{};
        desc.allocateType = RHIBufferAllocateType::eCPURead;
        desc.size         = sizeof(uint32_t);
        desc.usageFlags.SetFlags(RHIBufferUsageFlagBits::eStorageBuffer, RHIBufferUsageFlagBits::eTransferDstBuffer);
        RHIResourcePtr<RHIBuffer> output = RHIResourcePtr<RHIBuffer>::Adopt(device.CreateBuffer(desc));
        if (pipeline && output && shader->GetSRDByLocation(1, 0))
        {
            device.BeginFrame();
            ComputePass pass{device, *shader, *pipeline, *output};
            result = Run(pass);
        }
    }
    return result;
}
} // namespace example

int main()
{
    zen::RHIDeviceDesc desc;
    desc.SetValidationMode(zen::RHIValidationMode::eRequested);
    zen::RHIResult<zen::DynamicRHI*> backend = zen::CreateRHIChecked(desc);
    int                              status  = 1;
    if (backend)
    {
        zen::RHICommandListExecutor device(backend.GetValue(), zen::RHIExecutionMode::eThreaded);
#if defined(EXAMPLE_REQUIRES_RAY_QUERY)
        if (!device.QueryGPUInfo().rayQuery.IsUsable())
        {
            std::puts("Ray queries unavailable");
            status = 77;
        }
        else
#endif
        {
            status = example::Execute(device);
        }
    }
    else
    {
        std::fprintf(stderr, "%s failed (%lld)\n", backend.GetError().operation,
                     static_cast<long long>(backend.GetError().nativeCode));
    }
    return status;
}
