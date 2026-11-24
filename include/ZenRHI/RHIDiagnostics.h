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
using RHIMessageCallback = void (*)(RHIMessageSeverity severity, std::string_view message) noexcept;

// The device may report on any calling or worker thread. The view is valid for the duration of the callback.
void               ReportRHIMessage(RHIMessageSeverity severity, std::string_view message) noexcept;
RHIMessageCallback GetRHIMessageCallback() noexcept;
void               SetRHIMessageCallback(RHIMessageCallback callback) noexcept;

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
