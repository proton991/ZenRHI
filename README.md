# ZenRHI

ZenRHI supplies command recording, a threaded or inline executor and a Vulkan backend. The public CMake targets are ZenRHI::Core, ZenRHI::Vulkan and ZenRHI::ZenRHI. The combined target provides `CreateRHI`. Vulkan-only applications can link `ZenRHI::Vulkan`, call `CreateVulkanRHI`, and borrow the native instance and device with `GetVulkanDeviceHandles()`. The device must outlive every use of its borrowed handles.

Configure a standalone build with CMake 3.23 or newer, a C++20 compiler and a Vulkan SDK. Point CMAKE_PREFIX_PATH at an installed ZenFoundation, or set ZEN_FOUNDATION_SOURCE_DIR to a source checkout. Build and run CTest from the configured directory. The native Vulkan suite uses validation and a compatible driver.

Tests that share the fake backend or scoped Vulkan hooks can request `find_package(ZenRHI CONFIG REQUIRED COMPONENTS TestSupport)` and link `ZenRHI::TestSupport`. This component requires GoogleTest.

Top-level builds include Triangle and resolve SDL3 automatically. Set `ZENRHI_BUILD_EXAMPLES=OFF` to skip the example and its dependency. The example also configures independently against an installed ZenRHI. Run ZenRHITriangle to draw and present one frame.

See the [command recording, threading and lifetime contracts](include/ZenRHI/README.md).

A custom `RHIWindowHandle` borrows its extension and surface callbacks and window data. Keep them alive until the viewport is destroyed. Supply shader modules as owned SPIR-V bytes or through the copying span overload. The library receives all shader inputs from its caller.
