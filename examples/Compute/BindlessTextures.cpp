#include "Example.h"

namespace example
{
int Run(ComputePass& pass)
{
    RHITextureCreateInfo info{};
    info.format = DataFormat::eR8G8B8A8UNORM;
    info.type   = RHITextureType::e2D;
    info.width  = 42;
    info.usageFlags.SetFlag(RHITextureUsageFlagBits::eSampled);
    RHIResourcePtr<RHITexture> texture = RHIResourcePtr<RHITexture>::Adopt(pass.device.CreateTexture(info));
    RHIBindlessHandle          handle  = texture ? pass.device.RegisterBindlessResource(texture.Get(), 1) : RHIBindlessHandle{};
    RHICommandListPtr          commands = pass.Commands(RHICommandContextType::eGraphics);
    bool                       ok       = handle.IsValid() && commands;
    if (ok)
    {
        RHITextureTransition transition{};
        transition.pTexture      = texture.Get();
        transition.newUsage      = RHITextureUsage::eSampled;
        transition.newAccessMode = RHIAccessMode::eRead;
        transition.subResourceRange.aspect.SetFlag(RHITextureAspectFlagBits::eColor);
        commands->AddTransitions(RHIPipelineStageFlagBits::eTopOfPipe, RHIPipelineStageFlagBits::eComputeShader, {}, {},
                                 {&transition, 1});
        RHIBatchedShaderParameters parameters;
        pass.Dispatch(*commands, parameters);
        ok = pass.SubmitAndCheck(*commands, info.width);
    }
    if (handle.IsValid())
    {
        pass.device.UnregisterBindlessResource(handle);
    }
    return ok ? 0 : 1;
}
} // namespace example
