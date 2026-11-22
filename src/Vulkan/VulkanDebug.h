#pragma once
#include "ZenRHI/RHIDebug.h"

namespace zen
{
class VulkanDebug : public RHIDebug
{
public:
    explicit VulkanDebug();

    ~VulkanDebug() = default;

    void SetPipelineDebugName(RHIPipeline* pPipelineHandle, std::string_view debugName) final;

    void SetTextureDebugName(RHITexture* pTexture, std::string_view debugName) final;
};

} // namespace zen