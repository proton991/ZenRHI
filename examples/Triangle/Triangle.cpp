#include "ZenRHI/RHICommandListExecutor.h"
#include "ZenRHI/Vulkan/VulkanSurface.h"
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <cstdio>
#include <fstream>

namespace
{
using namespace zen;
std::span<const char* const> WindowExtensions()
{
    uint32_t           count = 0;
    const char* const* names = SDL_Vulkan_GetInstanceExtensions(&count);
    return {names, count};
}

VkResult MakeSurface(VkInstance instance, void* window, VkSurfaceKHR* surface)
{
    return SDL_Vulkan_CreateSurface(static_cast<SDL_Window*>(window), instance, nullptr, surface)
             ? VK_SUCCESS
             : VK_ERROR_INITIALIZATION_FAILED;
}

bool LoadStage(RHIShaderCreateInfo& info, RHIShaderStage stage, const char* path)
{
    std::ifstream        input(path, std::ios::binary | std::ios::ate);
    const std::streamoff size  = input ? static_cast<std::streamoff>(input.tellg()) : 0;
    bool                 valid = size > 0 && size % 4 == 0;
    if (valid)
    {
        std::vector<uint8_t> bytes(static_cast<size_t>(size));
        input.seekg(0);
        valid = static_cast<bool>(input.read(reinterpret_cast<char*>(bytes.data()), size));
        if (valid)
        {
            info.spirv->AppendOwnedStageSPIRV(stage, std::move(bytes));
            info.stageFlags.SetFlag(RHIShaderStageToFlagBits(stage));
        }
    }
    return valid;
}

bool DrawTriangle(RHICommandListExecutor& device, RHIViewport& viewport)
{
    RHIShaderCreateInfo shaders{};
    shaders.spirv              = MakeRefCountPtr<RHIShaderGroupSPIRV>();
    bool               success = LoadStage(shaders, RHIShaderStage::eVertex, TRIANGLE_SHADER_DIR "triangle.vert.spv")
                              && LoadStage(shaders, RHIShaderStage::eFragment, TRIANGLE_SHADER_DIR "triangle.frag.spv");
    RHIShader*         shader  = success ? device.CreateShader(shaders) : nullptr;
    RHIRenderingLayout layout{};
    layout.SetRenderArea(0, 0, viewport.GetWidth(), viewport.GetHeight());
    layout.AddColorRenderTarget(viewport.GetSwapchainFormat(), viewport.GetColorBackBuffer(), RHIRenderTargetLoadOp::eClear,
                                RHIRenderTargetStoreOp::eStore);
    RHIGfxPipelineCreateInfo info{};
    info.pShader              = shader;
    info.pRenderingLayout     = &layout;
    info.states.primitiveType = RHIDrawPrimitiveType::eTriangleList;
    info.states.dynamicStates.Enable(RHIDynamicState::eScissor, RHIDynamicState::eViewport);
    info.states.colorBlendState.AddAttachment();
    RHIPipeline* pipeline = shader ? device.CreatePipeline(info) : nullptr;
    success               = pipeline != nullptr;
    if (success)
    {
        device.BeginFrame();
        RHICommandListPtr commands(RHICommandList::Create(device.GetCommandContext(RHICommandContextType::eGraphics)));
        commands->BeginRendering(&layout);
        commands->SetViewport(0, 0, viewport.GetWidth(), viewport.GetHeight());
        commands->SetScissor(0, 0, viewport.GetWidth(), viewport.GetHeight());
        commands->BindPipeline(pipeline);
        commands->Draw(3, 1, 0, 0);
        commands->EndRendering();
        const RHIBatchResult result = device.SubmitFrame(*commands, &viewport).Wait();
        success                     = result.submission == RHISubmissionResult::eSuccess && result.presentation.presented;
        device.WaitDeviceIdle();
        device.DestroyPipeline(pipeline);
    }
    if (shader)
    {
        device.DestroyShader(shader);
    }
    return success;
}

bool Render(SDL_Window* window)
{
    RHIDeviceDesc description;
    description.surfaceExtensions = WindowExtensions;
    RHICommandListExecutor      device(CreateRHI(description), RHIExecutionMode::eThreaded);
    const VulkanSurfaceProvider provider{MakeSurface, nullptr};
    const RHIWindowHandle       handle{window, &provider};
    RHIViewport*                viewport = device.CreateViewport(handle, 640, 480, true);
    bool                        success  = viewport && viewport->GetColorBackBuffer() && DrawTriangle(device, *viewport);
    if (viewport)
    {
        device.DestroyViewport(viewport);
    }
    device.Destroy();
    return success;
}
} // namespace

int main()
{
    const bool  initialized = SDL_Init(SDL_INIT_VIDEO);
    SDL_Window* window      = initialized ? SDL_CreateWindow("ZenRHI Triangle", 640, 480, SDL_WINDOW_VULKAN) : nullptr;
    const bool  success     = window && Render(window);
    if (window)
    {
        SDL_DestroyWindow(window);
    }
    else
    {
        std::fprintf(stderr, "Window initialization failed: %s\n", SDL_GetError());
    }
    SDL_Quit();
    return success ? 0 : 1;
}
