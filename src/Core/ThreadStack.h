#pragma once
#include <cstdint>

namespace zen
{
// Windows x64 worker sample. The worker is resumed before symbol lookup or diagnostics.
void ReportWorkerStack(uint32_t threadID);
} // namespace zen
