#include "ZenRHI/DynamicRHI.h"

zen::DynamicRHI*   GDynamicRHI = nullptr;
zen::RHIFrameState GRHIFrameState;

namespace zen
{
namespace
{
std::atomic<RHIMessageCallback> messageCallback{};
}

RHIMessageCallback GetRHIMessageCallback() noexcept
{
    return messageCallback.load(std::memory_order_acquire);
}

void SetRHIMessageCallback(RHIMessageCallback callback) noexcept
{
    messageCallback.store(callback, std::memory_order_release);
}

void ReportRHIMessage(RHIMessageSeverity severity, std::string_view message) noexcept
{
    const RHIMessageCallback callback = messageCallback.load(std::memory_order_acquire);
    if (callback)
    {
        callback(severity, message);
    }
    else
    {
        const char* label = severity == RHIMessageSeverity::eInfo    ? "info"
                          : severity == RHIMessageSeverity::eWarning ? "warning"
                                                                     : "error";
        std::fprintf(stderr, "[%s] %.*s\n", label, static_cast<int>(message.size()), message.empty() ? "" : message.data());
    }
}
} // namespace zen
