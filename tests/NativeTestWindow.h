#pragma once

#include "ZenRHI/RHIWindowHandle.h"
#include <cstdint>

namespace zen::test
{
class NativeTestWindow
{
public:
    NativeTestWindow(uint32_t width, uint32_t height);
    ~NativeTestWindow();
    NativeTestWindow(const NativeTestWindow&)            = delete;
    NativeTestWindow& operator=(const NativeTestWindow&) = delete;

    RHIWindowHandle GetHandle() const;
    void            Resize(uint32_t width, uint32_t height);

private:
    void* m_window{};
};
} // namespace zen::test
