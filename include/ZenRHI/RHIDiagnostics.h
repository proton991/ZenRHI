#pragma once

#include "ZenFoundation/Utils/Errors.h"
#include <string_view>

namespace zen
{
enum class RHIMessageSeverity
{
    eInfo,
    eWarning,
    eError
};
// One diagnostic. Validation-layer messages carry their message id (a VUID or a hazard name); RHI logs carry none.
// The views are valid for the duration of the callback.
struct RHIMessage
{
    RHIMessageSeverity severity{RHIMessageSeverity::eInfo};
    std::string_view   id;
    std::string_view   text;
};

using RHIMessageCallback = void (*)(const RHIMessage& message) noexcept;

// The device may report on any calling or worker thread. Without a callback, messages go to stderr.
void               ReportRHIMessage(const RHIMessage& message) noexcept;
void               ReportRHIMessage(RHIMessageSeverity severity, std::string_view text) noexcept;
RHIMessageCallback GetRHIMessageCallback() noexcept;
void               SetRHIMessageCallback(RHIMessageCallback callback) noexcept;

// RHI resources or command contexts that outlive backend teardown are ownership bugs. Strict checks
// abort through the verification path; otherwise teardown logs and continues, so a leak does not turn
// a shipping build's exit into a crash. Process-wide, because the checks also run after the backend
// is gone. Debug builds default to strict.
void SetRHIStrictTeardownChecks(bool enabled) noexcept;
bool GetRHIStrictTeardownChecks() noexcept;

[[noreturn]] inline void RHIVerificationFailure(const char* expression, const char* file, int line, const char* message)
{
    ReportRHIMessage(RHIMessageSeverity::eError, message ? message : expression);
    VerificationFailure(expression, file, line, message);
}

template <typename... Arguments> [[noreturn]] void RHIVerificationFailureFormatted(const char*                      expression,
                                                                                   const char*                      file,
                                                                                   int                              line,
                                                                                   fmt::format_string<Arguments...> format,
                                                                                   Arguments&&... arguments)
{
    char                                 message[1024]{};
    const fmt::format_to_n_result<char*> result =
        fmt::format_to_n(message, sizeof(message) - 1, format, std::forward<Arguments>(arguments)...);
    message[std::min(result.size, sizeof(message) - 1)] = '\0';
    RHIVerificationFailure(expression, file, line, message);
}
} // namespace zen

#define ZEN_RHI_LOG_INFO(...)  ::zen::ReportRHIMessage(::zen::RHIMessageSeverity::eInfo, fmt::format(__VA_ARGS__));
#define ZEN_RHI_LOG_WARN(...)  ::zen::ReportRHIMessage(::zen::RHIMessageSeverity::eWarning, fmt::format(__VA_ARGS__));
#define ZEN_RHI_LOG_ERROR(...) ::zen::ReportRHIMessage(::zen::RHIMessageSeverity::eError, fmt::format(__VA_ARGS__));
#define ZEN_RHI_VERIFY(x)      ZEN_RHI_VERIFY_MSG(x, nullptr)
#define ZEN_RHI_VERIFY_MSG(x, message)                                      \
    do                                                                      \
    {                                                                       \
        if (!(x))                                                           \
            ::zen::RHIVerificationFailure(#x, __FILE__, __LINE__, message); \
    } while (0)
#define ZEN_RHI_VERIFY_MSG_F(x, ...)                                                     \
    do                                                                                   \
    {                                                                                    \
        if (!(x))                                                                        \
            ::zen::RHIVerificationFailureFormatted(#x, __FILE__, __LINE__, __VA_ARGS__); \
    } while (0)
