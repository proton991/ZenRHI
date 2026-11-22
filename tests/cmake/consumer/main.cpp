#include <ZenRHI/RHICommandListExecutor.h>

// Reference the factory so the consumer links every backend archive without requiring a GPU at configure time.
int main(int argc, char**)
{
    if (argc > 1)
    {
        zen::DynamicRHI*            device = zen::CreateRHI(zen::RHIDeviceDesc{});
        zen::RHICommandListExecutor executor(device, zen::RHIExecutionMode::eThreaded);
        executor.Destroy();
    }
    return 0;
}
