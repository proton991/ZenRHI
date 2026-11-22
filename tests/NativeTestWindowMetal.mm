#include "NativeTestWindow.h"
#import <AppKit/AppKit.h>
#import <QuartzCore/CAMetalLayer.h>

namespace zen::test
{
NativeTestWindow::NativeTestWindow(uint32_t width, uint32_t height)
{
    @autoreleasepool
    {
        [NSApplication sharedApplication];
        NSWindow* window              = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, width, height)
                                                                    styleMask:NSWindowStyleMaskBorderless
                                                                      backing:NSBackingStoreBuffered
                                                                        defer:NO];
        window.releasedWhenClosed     = NO;
        window.contentView.wantsLayer = YES;
        window.contentView.layer      = [CAMetalLayer layer];
        m_window                      = window;
        Resize(width, height);
    }
}

NativeTestWindow::~NativeTestWindow()
{
    @autoreleasepool
    {
        NSWindow* window = static_cast<NSWindow*>(m_window);
        [window close];
        [window release];
    }
}

RHIWindowHandle NativeTestWindow::GetHandle() const
{
    NSWindow* window = static_cast<NSWindow*>(m_window);
    return {RHIWindowType::eMetal, window.contentView.layer};
}

void NativeTestWindow::Resize(uint32_t width, uint32_t height)
{
    @autoreleasepool
    {
        NSWindow* window = static_cast<NSWindow*>(m_window);
        [window setContentSize:NSMakeSize(width, height)];
        CAMetalLayer* layer = (CAMetalLayer*)window.contentView.layer;
        layer.drawableSize  = CGSizeMake(width, height);
    }
}
} // namespace zen::test
