#include "Example.h"

namespace example
{
int Run(ComputePass& pass)
{
    const RHICommandContextType queues[] = {RHICommandContextType::eGraphics, RHICommandContextType::eAsyncCompute};
    int                         status   = 77;
    if (pass.device.GetQueueCapabilities().asyncSubmissionDependencies)
    {
        RHICommandListPtr producer = pass.Commands(queues[0]);
        RHICommandListPtr consumer = pass.Commands(queues[1]);
        status                     = 1;
        if (producer && consumer)
        {
            producer->ClearBuffer(&pass.output, 0, sizeof(uint32_t));
            RHIBatchedShaderParameters parameters;
            pass.Dispatch(*consumer, parameters);
            RHISubmissionStatePtr state    = MakeRefCountPtr<RHISubmissionState>(queues);
            RHISubmissionGroup    groups[] = {{producer.get(), {}}, {consumer.get(), {{queues[0], 0, state, 0}}}};
            status                         = pass.Check(pass.device.SubmitFrame(groups, nullptr, state).Wait(), 42) ? 0 : 1;
        }
    }
    else
    {
        std::puts("Async submission dependencies unavailable");
    }
    return status;
}
} // namespace example
