#include "NativeTestWindow.h"
#include <windows.h>

namespace zen::test
{
NativeTestWindow::NativeTestWindow(uint32_t width, uint32_t height)
{
    m_window = CreateWindowExW(0, L"STATIC", L"ZenRHI test", WS_POPUP, 0, 0, static_cast<int>(width), static_cast<int>(height),
                               nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
}

NativeTestWindow::~NativeTestWindow()
{
    if (m_window)
    {
        DestroyWindow(static_cast<HWND>(m_window));
    }
}

RHIWindowHandle NativeTestWindow::GetHandle() const
{
    return {RHIWindowType::eWin32, m_window, GetModuleHandleW(nullptr)};
}

void NativeTestWindow::Resize(uint32_t width, uint32_t height)
{
    SetWindowPos(static_cast<HWND>(m_window), nullptr, 0, 0, static_cast<int>(width), static_cast<int>(height),
                 SWP_NOMOVE | SWP_NOACTIVATE | SWP_NOZORDER);
}
} // namespace zen::test
