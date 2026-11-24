#include "Example.h"

namespace example
{
int Run(ComputePass& pass)
{
    RHICommandListPtr commands = pass.Commands(RHICommandContextType::eGraphics);
    bool              ok       = commands != nullptr;
    if (ok)
    {
        RHIBatchedShaderParameters parameters;
        pass.Dispatch(*commands, parameters);
        ok = pass.SubmitAndCheck(*commands, 42);
    }
    return ok ? 0 : 1;
}
} // namespace example
