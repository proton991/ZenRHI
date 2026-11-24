#pragma once
#include "ZenRHI/RHIDiagnostics.h"
#include <cstdint>

namespace zen::test
{
// Query the active backend between creation and teardown. An empty ID matches all messages.
uint64_t GetValidationCount(RHIMessageSeverity severity, std::string_view id = {});
// Includes messages emitted during device teardown and failed startup.
uint64_t GetProcessValidationErrors();
} // namespace zen::test
