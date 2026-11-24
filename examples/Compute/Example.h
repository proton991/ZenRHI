#pragma once
#include <ZenRHI/RHICommandListExecutor.h>

namespace example
{
using namespace zen;

// The host owns these objects until the example has waited for its submission.
struct ComputePass
{
    RHICommandListExecutor& device;
    RHIShader&              shader;
    RHIPipeline&            pipeline;
    RHIBuffer&              output;

    RHICommandListPtr Commands(RHICommandContextType queue);
    void              Dispatch(RHICommandList& commands, RHIBatchedShaderParameters& parameters);
    bool              SubmitAndCheck(RHICommandList& commands, uint32_t expected);
    bool              Check(const RHIBatchResult& batch, uint32_t expected);
};

int Run(ComputePass& pass);
} // namespace example
