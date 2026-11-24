#pragma once
#include "ZenRHI/RHIDiagnostics.h"

namespace zen
{
class VulkanFaultReport
{
public:
    template <typename... Args> void Append(fmt::format_string<Args...> format, Args&&... args)
    {
        const size_t                         remaining = sizeof(m_text) - m_size;
        const fmt::format_to_n_result<char*> result =
            fmt::format_to_n(m_text + m_size, remaining, format, std::forward<Args>(args)...);
        m_size      += std::min(remaining, result.size);
        m_truncated |= remaining < result.size;
    }

    void Publish()
    {
        constexpr std::string_view marker = "\n[report truncated]";
        if (m_truncated)
        {
            std::copy(marker.begin(), marker.end(), m_text + sizeof(m_text) - marker.size());
        }
        ReportRHIMessage(RHIMessageSeverity::eError, {m_text, m_size});
    }

private:
    char   m_text[16384];
    size_t m_size{};
    bool   m_truncated{};
};
} // namespace zen
