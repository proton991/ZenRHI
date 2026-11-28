#include "RHIValidation.h"
#include "ZenRHI/RHIFormat.h"

namespace zen
{
namespace
{
bool IsClampAddressMode(RHISamplerRepeatMode mode)
{
    return mode == RHISamplerRepeatMode::eClampToEdge || mode == RHISamplerRepeatMode::eClampToBorder;
}
} // namespace

bool IsTextureShapeValid(const RHITextureCreateInfo& info)
{
    const bool extents = info.width > 0 && info.height > 0 && info.depth > 0 && info.arrayLayers > 0 && info.mipmaps > 0;
    bool       shape   = false;
    switch (info.type)
    {
        case RHITextureType::e1D: shape = info.height == 1 && info.depth == 1; break;
        case RHITextureType::e2D: shape = info.depth == 1; break;
        case RHITextureType::e3D: shape = info.arrayLayers == 1; break;
        case RHITextureType::eCube: shape = info.width == info.height && info.depth == 1 && info.arrayLayers % 6 == 0; break;
        default: break;
    }
    const bool samples = info.samples < SampleCount::eMax
                      && (info.samples == SampleCount::e1 || (info.type == RHITextureType::e2D && info.mipmaps == 1));
    return extents && shape && samples
        && info.mipmaps <= RHITexture::CalculateTextureMipLevels(info.width, info.height, info.depth);
}

const char* ValidateBufferCreateInfo(const RHIBufferCreateInfo& info, const RHIGPUInfo& gpu)
{
    const char* rule = nullptr;
    if (info.allocateType <= RHIBufferAllocateType::eNone || info.allocateType >= RHIBufferAllocateType::eMax)
    {
        rule = "invalid allocation type";
    }
    else if (info.size == 0 || info.usageFlags.IsEmpty())
    {
        rule = "size and usage must be specified";
    }
    else if (info.usageFlags.HasFlag(RHIBufferUsageFlagBits::eDeviceAddress) && !gpu.rayQuery.bufferDeviceAddress)
    {
        rule = "buffer device addresses are unsupported";
    }
    else if (info.usageFlags.HasFlag(RHIBufferUsageFlagBits::eShaderBindingTable) && !gpu.rayTracing.IsUsable())
    {
        rule = "shader binding tables need ray-tracing pipelines";
    }
    return rule;
}

const char* ValidateTextureCreateInfo(const RHITextureCreateInfo& info, const RHIGPUInfo& gpu)
{
    const char* rule = nullptr;
    if (info.format == DataFormat::eUndefined || GetTextureFormatBlockInfo(info.format).bytes == 0 || info.usageFlags.IsEmpty())
    {
        rule = "format and usage must be specified";
    }
    else if (!IsTextureShapeValid(info))
    {
        rule = "invalid extent, type, sample count or mip count";
    }
    else if (info.type == RHITextureType::eCube && info.arrayLayers > 6 && !gpu.supportImageCubeArray)
    {
        rule = "cube arrays are unsupported";
    }
    return rule;
}

const char* ValidateTextureViewCreateInfo(const RHITextureCreateInfo&     image,
                                          const RHITextureViewCreateInfo& view,
                                          const RHIGPUInfo&               gpu)
{
    const char* rule = nullptr;
    if (view.mipLevels == 0 || view.baseMipLevel >= image.mipmaps || view.mipLevels > image.mipmaps - view.baseMipLevel
        || view.arrayLayers == 0 || view.baseArrayLayer >= image.arrayLayers
        || view.arrayLayers > image.arrayLayers - view.baseArrayLayer)
    {
        rule = "Texture view mip or array-layer range is outside its image";
    }
    else if (view.type != image.type && !(image.type == RHITextureType::eCube && view.type == RHITextureType::e2D))
    {
        rule = "Texture view type is incompatible with its image";
    }
    else if (view.type == RHITextureType::eCube && view.arrayLayers % 6 != 0)
    {
        rule = "Cube views require a multiple of six array layers";
    }
    else if (view.type == RHITextureType::e3D && view.arrayView)
    {
        rule = "3D texture views cannot be array views";
    }
    else if (view.type == RHITextureType::eCube && (view.arrayView || view.arrayLayers > 6) && !gpu.supportImageCubeArray)
    {
        rule = "Cube-array views require imageCubeArray";
    }
    else if ((int64_t(view.aspect) & ~int64_t(GetTextureFormatAspects(view.format))) != 0)
    {
        rule = "Texture view aspects are incompatible with its format";
    }
    return rule;
}

const char* ValidateSamplerCreateInfo(const RHISamplerCreateInfo& info, const RHIGPUInfo& gpu)
{
    const bool mirror = info.repeatU == RHISamplerRepeatMode::eMirrorClampToEdge
                     || info.repeatV == RHISamplerRepeatMode::eMirrorClampToEdge
                     || info.repeatW == RHISamplerRepeatMode::eMirrorClampToEdge;
    const bool unnormalized =
        !info.unnormalizedUVW
        || (info.minFilter == info.magFilter && info.mipFilter == RHISamplerFilter::eNearest && info.minLod == 0
            && info.maxLod == 0 && !info.useAnisotropy && !info.enableCompare && IsClampAddressMode(info.repeatU)
            && IsClampAddressMode(info.repeatV) && IsClampAddressMode(info.repeatW));
    const char* rule = nullptr;
    if (mirror && !gpu.supportSamplerMirrorClampToEdge)
    {
        rule = "mirror-clamp addressing is unsupported";
    }
    else if (!unnormalized)
    {
        rule = "unnormalized coordinates need equal filters, no mips, LOD 0, clamping, and no anisotropy or compare";
    }
    else if (info.maxLod < info.minLod)
    {
        rule = "maxLod is below minLod";
    }
    return rule;
}

const char* ValidateShaderStages(const RHIShaderGroupSPIRV* spirv, const RHIGPUInfo& gpu)
{
    bool rasterOrCompute = false;
    bool rayTracing      = false;
    for (uint32_t i = 0; spirv != nullptr && i < spirv->GetModules().size(); ++i)
    {
        rasterOrCompute |= spirv->GetModules()[i].stage < RHIShaderStage::eRayGeneration;
        rayTracing      |= spirv->GetModules()[i].stage >= RHIShaderStage::eRayGeneration;
    }
    const char* rule = nullptr;
    if (spirv == nullptr || spirv->GetModules().empty())
    {
        rule = "a shader needs at least one stage";
    }
    else if (spirv->HasShaderStage(RHIShaderStage::eTesselationControl)
             || spirv->HasShaderStage(RHIShaderStage::eTesselationEvaluation))
    {
        rule = "tessellation is not part of the RHI pipeline contract";
    }
    else if (rayTracing && (rasterOrCompute || !gpu.rayTracing.IsUsable()))
    {
        rule = "ray-tracing stages need enabled ray-tracing pipelines and no other stages";
    }
    return rule;
}
} // namespace zen
