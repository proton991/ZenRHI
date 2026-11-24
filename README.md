# ZenRHI

ZenRHI supplies command recording, a threaded or inline executor and a Vulkan backend. The public CMake targets are ZenRHI::Core, ZenRHI::Vulkan and ZenRHI::ZenRHI. The combined target provides `CreateRHI`. Vulkan-only applications can link `ZenRHI::Vulkan`, call `CreateVulkanRHI`, and borrow the native instance and device with `GetVulkanDeviceHandles()`. The device must outlive every use of its borrowed handles.

Configure a standalone build with CMake 3.24 or newer, a C++20 compiler and a Vulkan SDK with SPIRV-Tools. Point CMAKE_PREFIX_PATH at an installed ZenFoundation, or set ZEN_FOUNDATION_SOURCE_DIR to a source checkout. Build and run CTest from the configured directory. The native Vulkan suite uses validation and a compatible driver.

Tests that share the fake backend or scoped Vulkan hooks can request `find_package(ZenRHI CONFIG REQUIRED COMPONENTS TestSupport)` and link `ZenRHI::TestSupport`. This component requires GoogleTest.

Top-level builds include Triangle and four compute examples, and resolve SDL3 for Triangle. Set `ZENRHI_BUILD_EXAMPLES=OFF` to skip example builds. Each example directory also configures independently against an installed ZenRHI. Run ZenRHITriangle to draw and present one frame.

See the [command recording, threading and lifetime contracts](include/ZenRHI/README.md).

A custom `RHIWindowHandle` borrows its extension and surface callbacks and window data. Keep them alive until the viewport is destroyed. Supply shader modules as owned SPIR-V bytes or through the copying span overload. The library receives all shader inputs from its caller.

## SDK compatibility

From 1.0 onward, a breaking change to the public API requires a major version increment. A public removal must spend at least one minor release deprecated first. Release reviewers inspect the public header diff from the previous version tag, including changes to defaults, enum values and type layouts. The policy job also checks removed or changed declarations.

See the [programming guide](ProgrammingGuide.md) for checked startup, submission and shutdown. The [compute examples](examples/Compute) each run a shader and check its GPU result. Configure that directory against the installed package to build all four executables.
