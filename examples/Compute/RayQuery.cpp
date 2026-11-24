#include "Example.h"

namespace example
{
int Run(ComputePass& pass)
{
    // Build an empty scene. The shader verifies that its ray misses.
    RHIAccelerationStructureBuildInfo build{};
    build.description.type                         = RHIAccelerationStructureType::eTopLevel;
    const RHIAccelerationStructureBuildSizes sizes = pass.device.GetAccelerationStructureBuildSizes(build.description);
    RHIAccelerationStructureCreateInfo       info{};
    info.type = build.description.type;
    info.size = sizes.storageSize;
    RHIResourcePtr<RHIAccelerationStructure> scene =
        RHIResourcePtr<RHIAccelerationStructure>::Adopt(pass.device.CreateAccelerationStructure(info));
    const uint64_t      alignment = pass.device.QueryGPUInfo().rayQuery.scratchAlignment;
    RHIBufferCreateInfo desc{};
    desc.size         = sizes.buildScratchSize + alignment;
    desc.allocateType = RHIBufferAllocateType::eGPU;
    desc.usageFlags.SetFlags(RHIBufferUsageFlagBits::eStorageBuffer, RHIBufferUsageFlagBits::eDeviceAddress);
    RHIResourcePtr<RHIBuffer> scratch  = RHIResourcePtr<RHIBuffer>::Adopt(pass.device.CreateBuffer(desc));
    RHICommandListPtr         commands = pass.Commands(RHICommandContextType::eGraphics);
    bool                      ok       = sizes.IsValid() && alignment && scene && scratch && commands;
    if (ok)
    {
        build.pDestination   = scene.Get();
        build.pScratchBuffer = scratch.Get();
        build.scratchOffset  = (alignment - scratch->GetDeviceAddress() % alignment) % alignment;
        commands->BuildAccelerationStructure(build);
        RHIMemoryTransition barrier{RHIAccessFlagBits::eAccelerationStructureWrite,
                                    RHIAccessFlagBits::eAccelerationStructureRead};
        commands->AddTransitions(RHIPipelineStageFlagBits::eAccelerationStructureBuild,
                                 RHIPipelineStageFlagBits::eComputeShader, {&barrier, 1}, {}, {});
        RHIBatchedShaderParameters parameters;
        parameters.AddResourceParam(*pass.shader.GetSRDByLocation(1, 1), scene.Get(), nullptr, 0);
        pass.Dispatch(*commands, parameters);
        ok = pass.SubmitAndCheck(*commands, 42);
    }
    return ok ? 0 : 1;
}
} // namespace example
