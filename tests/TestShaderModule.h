#pragma once
#include "ZenRHI/RHIResource.h"
#include <filesystem>
#include <fstream>
namespace zen::test
{
inline void LoadShaderModule(RHIShaderCreateInfo& info, RHIShaderStage stage, const std::filesystem::path& path)
{
    std::ifstream        input(path, std::ios::binary | std::ios::ate);
    const std::streamoff size = input ? static_cast<std::streamoff>(input.tellg()) : 0;
    ZEN_VERIFY_MSG(size > 0 && size % 4 == 0, "Invalid test shader");
    HeapVector<uint8_t> bytes(static_cast<size_t>(size));
    input.seekg(0);
    ZEN_VERIFY(input.read(reinterpret_cast<char*>(bytes.data()), size));
    if (!info.spirv)
    {
        info.spirv = MakeRefCountPtr<RHIShaderGroupSPIRV>();
    }
    info.spirv->SetStageSPIRV(stage, std::move(bytes));
    info.stageFlags.SetFlag(RHIShaderStageToFlagBits(stage));
    info.spirvFileName[ToUnderlying(stage)] = path.filename().string();
}
} // namespace zen::test
