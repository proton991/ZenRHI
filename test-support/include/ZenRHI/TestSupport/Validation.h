#pragma once
#include "ZenRHI/RHIDiagnostics.h"
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

namespace zen::test
{
// Counts validation messages, which carry a message id, by severity and id, and prints every message. One per process,
// so counts include device teardown and failed startup.
class ValidationCounter
{
public:
    static ValidationCounter& Get()
    {
        static ValidationCounter counter;
        return counter;
    }

    // Replaces the process message callback.
    static void Install()
    {
        SetRHIMessageCallback(&ValidationCounter::Receive);
    }

    // Counts of one severity; an empty id sums every id.
    uint64_t Count(RHIMessageSeverity severity, std::string_view id = {})
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        uint64_t                    total = 0;
        for (const Entry& entry : m_entries)
        {
            total += entry.severity == severity && (id.empty() || id == entry.id) ? entry.count : 0;
        }
        return total;
    }

private:
    struct Entry
    {
        RHIMessageSeverity severity;
        std::string        id;
        uint64_t           count;
    };

    static void Receive(const RHIMessage& message) noexcept
    {
        if (!message.id.empty())
        {
            Get().Add(message.severity, message.id);
        }
        const char* label = message.severity == RHIMessageSeverity::eInfo    ? "info"
                          : message.severity == RHIMessageSeverity::eWarning ? "warning"
                                                                             : "error";
        std::fprintf(stderr, "[%s] %.*s\n", label, static_cast<int>(message.text.size()),
                     message.text.empty() ? "" : message.text.data());
    }

    void Add(RHIMessageSeverity severity, std::string_view id)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        bool                        found = false;
        for (Entry& entry : m_entries)
        {
            if (!found && entry.severity == severity && entry.id == id)
            {
                ++entry.count;
                found = true;
            }
        }
        if (!found)
        {
            m_entries.push_back({severity, std::string(id), 1});
        }
    }

    std::mutex         m_mutex;
    std::vector<Entry> m_entries;
};

// Installs the counter for a scope, for a test that counts messages under a host's own callback.
class ScopedValidationCounter
{
public:
    ScopedValidationCounter() : m_previous(GetRHIMessageCallback())
    {
        ValidationCounter::Install();
    }

    ~ScopedValidationCounter()
    {
        SetRHIMessageCallback(m_previous);
    }

    ScopedValidationCounter(const ScopedValidationCounter&)            = delete;

    ScopedValidationCounter& operator=(const ScopedValidationCounter&) = delete;

private:
    RHIMessageCallback m_previous;
};

inline uint64_t GetProcessValidationErrors()
{
    return ValidationCounter::Get().Count(RHIMessageSeverity::eError);
}
} // namespace zen::test
