#pragma once

#include "ZenRHI/RHIWindowHandle.h"
#include <cstdint>
#include <span>

namespace zen::test
{
class NativeTestWindow
{
public:
    NativeTestWindow(uint32_t width, uint32_t height);
    ~NativeTestWindow();
    NativeTestWindow(const NativeTestWindow&)            = delete;
    NativeTestWindow& operator=(const NativeTestWindow&) = delete;

    // The instance extensions its surface provider needs.
    static std::span<const char* const> InstanceExtensions()
    {
        static constexpr const char* names[] = {"VK_KHR_win32_surface"};
        return names;
    }

    RHIWindowHandle GetHandle() const;
    void            Resize(uint32_t width, uint32_t height);

private:
    void* m_window{};
};
} // namespace zen::test
