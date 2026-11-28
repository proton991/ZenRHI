#include "ZenRHI/RHIDiagnostics.h"
#include "ZenRHI/RHIError.h"
#include <atomic>
#include <cstdio>

namespace zen
{
namespace
{
std::atomic<RHIMessageCallback> messageCallback{};
#if defined(NDEBUG)
std::atomic<bool> strictTeardownChecks{false};
#else
std::atomic<bool> strictTeardownChecks{true};
#endif
} // namespace

RHIMessageCallback GetRHIMessageCallback() noexcept
{
    return messageCallback.load(std::memory_order_acquire);
}

void SetRHIMessageCallback(RHIMessageCallback callback) noexcept
{
    messageCallback.store(callback, std::memory_order_release);
}

void SetRHIStrictTeardownChecks(bool enabled) noexcept
{
    strictTeardownChecks.store(enabled, std::memory_order_relaxed);
}

bool GetRHIStrictTeardownChecks() noexcept
{
    return strictTeardownChecks.load(std::memory_order_relaxed);
}

void ReportRHIMessage(const RHIMessage& message) noexcept
{
    const RHIMessageCallback callback = messageCallback.load(std::memory_order_acquire);
    if (callback)
    {
        callback(message);
    }
    else
    {
        const char* label = message.severity == RHIMessageSeverity::eInfo    ? "info"
                          : message.severity == RHIMessageSeverity::eWarning ? "warning"
                                                                             : "error";
        std::fprintf(stderr, "[%s] %.*s\n", label, static_cast<int>(message.text.size()),
                     message.text.empty() ? "" : message.text.data());
    }
}

void ReportRHIMessage(RHIMessageSeverity severity, std::string_view text) noexcept
{
    ReportRHIMessage(RHIMessage{severity, {}, text});
}
} // namespace zen

namespace zen
{
void VerifyTeardownOwnership(bool released, const char* message, const char* pFile, int line)
{
    if (!released)
    {
        if (GetRHIStrictTeardownChecks())
        {
            VerificationFailure("teardown ownership", pFile, line, message);
        }
        else
        {
            ZEN_RHI_LOG_ERROR("{}", message);
        }
    }
}
} // namespace zen
