#pragma once
#include "DynamicRHI.h"
#include "ZenFoundation/Templates/NameID.h"

namespace zen
{
class RHIDebug
{
public:
    static RHIDebug* Create();

    virtual ~RHIDebug()                                                                         = default;

    virtual void SetPipelineDebugName(RHIPipeline* pPipelineHandle, std::string_view debugName) = 0;

    virtual void SetTextureDebugName(RHITexture* pTexture, std::string_view debugName)          = 0;

protected:
    RHIDebug() {}
};
} // namespace zen