cmake_minimum_required(VERSION 3.23)
project(ZenRHI VERSION 0.2.0 LANGUAGES C CXX)
# Zen components are released together: each one requires the matching ZenFoundation major.minor.
set(ZENRHI_FOUNDATION_ABI "${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}")
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
include(FetchContent)
option(ZENRHI_BUILD_TESTS "Build RHI tests" ${PROJECT_IS_TOP_LEVEL})
option(ZENRHI_BUILD_EXAMPLES "Build the SDL3 Triangle example" ${PROJECT_IS_TOP_LEVEL})
option(ZENRHI_INSTALL "Install ZenRHI and CMake package files" ${PROJECT_IS_TOP_LEVEL})

if(NOT TARGET ZenFoundation::Foundation)
    if(ZENRHI_INSTALL AND NOT DEFINED ZENFOUNDATION_INSTALL)
        set(ZENFOUNDATION_INSTALL ON)
    endif()
    if(ZEN_FOUNDATION_SOURCE_DIR)
        add_subdirectory("${ZEN_FOUNDATION_SOURCE_DIR}" "${PROJECT_BINARY_DIR}/ZenFoundation")
    else()
        find_package(ZenFoundation ${ZENRHI_FOUNDATION_ABI} CONFIG QUIET)
        if(NOT TARGET ZenFoundation::Foundation)
            include(FetchContent)
            FetchContent_Declare(ZenFoundation GIT_REPOSITORY https://github.com/proton991/ZenFoundation.git
                GIT_TAG v${PROJECT_VERSION} GIT_SHALLOW TRUE)
            FetchContent_MakeAvailable(ZenFoundation)
        endif()
    endif()
endif()
get_target_property(foundation_abi ZenFoundation::Foundation INTERFACE_ZenFoundation_ABI)
if(NOT foundation_abi STREQUAL ZENRHI_FOUNDATION_ABI)
    message(FATAL_ERROR "${PROJECT_NAME} ${PROJECT_VERSION} requires ZenFoundation ${ZENRHI_FOUNDATION_ABI}.x, found ABI ${foundation_abi}")
endif()
find_package(Vulkan REQUIRED)
add_library(ZenRHICore STATIC
    src/Core/DynamicRHI.cpp
    src/Core/RHICommandList.cpp
    src/Core/RHICommandListExecutor.cpp
    src/Core/RHIGPUFrameTiming.cpp
    src/Core/RHIGlobals.cpp
    src/Core/RHIRayTracing.cpp
    src/Core/RHIResource.cpp
    src/Core/RHIShaderUtil.cpp
    src/Core/RHIThread.cpp)
add_library(ZenRHIVulkan STATIC
    src/Vulkan/Platform/VulkanMacOSPlatform.cpp
    src/Vulkan/Platform/VulkanWindowBridge.cpp
    src/Vulkan/Platform/VulkanWindowsPlatform.cpp
    src/Vulkan/VulkanAccelerationStructure.cpp
    src/Vulkan/VulkanBuffer.cpp
    src/Vulkan/VulkanCommandList.cpp
    src/Vulkan/VulkanContext.cpp
    src/Vulkan/VulkanDebug.cpp
    src/Vulkan/VulkanDescriptorPool.cpp
    src/Vulkan/VulkanDescriptorSetState.cpp
    src/Vulkan/VulkanDevice.cpp
    src/Vulkan/VulkanExtension.cpp
    src/Vulkan/VulkanLifetimeTracker.cpp
    src/Vulkan/VulkanMemory.cpp
    src/Vulkan/VulkanPipeline.cpp
    src/Vulkan/VulkanQueue.cpp
    src/Vulkan/VulkanSwapchain.cpp
    src/Vulkan/VulkanSynchronization.cpp
    src/Vulkan/VulkanTexture.cpp
    src/Vulkan/VulkanTypes.cpp
    src/Vulkan/VulkanRHIFactory.cpp
    src/Vulkan/VulkanViewport.cpp
    src/Vulkan/vk_mem_alloc.cpp)
add_library(ZenRHI STATIC src/RHIFactory.cpp)
foreach(part IN ITEMS Core Vulkan)
    add_library(ZenRHI::${part} ALIAS ZenRHI${part})
    set_target_properties(ZenRHI${part} PROPERTIES EXPORT_NAME ${part})
endforeach()
add_library(ZenRHI::ZenRHI ALIAS ZenRHI)
add_library(ZenRHITestSupport INTERFACE)
add_library(ZenRHI::TestSupport ALIAS ZenRHITestSupport)
set_target_properties(ZenRHITestSupport PROPERTIES EXPORT_NAME TestSupport)
target_link_libraries(ZenRHITestSupport INTERFACE ZenRHI::Core $<BUILD_INTERFACE:GTest::gtest>)
target_include_directories(ZenRHITestSupport INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/test-support/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
set_property(TARGET ZenRHICore PROPERTY ZenFoundation_ABI "${ZENRHI_FOUNDATION_ABI}")
target_compile_features(ZenRHICore PUBLIC cxx_std_20)
target_include_directories(ZenRHICore PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_include_directories(ZenRHIVulkan PRIVATE src "${Vulkan_INCLUDE_DIRS}")
target_include_directories(ZenRHI PRIVATE src "${Vulkan_INCLUDE_DIRS}")
target_link_libraries(ZenRHICore PUBLIC ZenFoundation::Foundation)
target_link_libraries(ZenRHIVulkan PUBLIC ZenRHICore Vulkan::Headers)
target_link_libraries(ZenRHI PUBLIC ZenRHIVulkan)
target_compile_definitions(ZenRHIVulkan PRIVATE VK_NO_PROTOTYPES)
target_compile_definitions(ZenRHI PRIVATE VK_NO_PROTOTYPES)
include(cmake/BackendDependencies.cmake)

zen_add_version(ZenRHICore ZenRHI "${PROJECT_SOURCE_DIR}" "${PROJECT_VERSION}")

if(ZENRHI_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
if(ZENRHI_BUILD_EXAMPLES)
    add_subdirectory(examples/Triangle)
endif()
if(ZENRHI_INSTALL)
    set(package_dir "${CMAKE_INSTALL_LIBDIR}/cmake/ZenRHI")
    install(FILES "${CMAKE_CURRENT_BINARY_DIR}/generated/ZenRHI/Version.h" DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/ZenRHI")
    install(TARGETS ZenRHICore ZenRHIVulkan ZenRHI EXPORT ZenRHITargets)
    install(DIRECTORY include/ DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}" PATTERN "*.md" EXCLUDE)
    install(TARGETS ZenRHITestSupport EXPORT ZenRHITestSupportTargets)
    install(DIRECTORY test-support/include/ DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
    install(EXPORT ZenRHITestSupportTargets NAMESPACE ZenRHI:: DESTINATION "${package_dir}")
    install(EXPORT ZenRHITargets NAMESPACE ZenRHI:: DESTINATION "${package_dir}")
    configure_package_config_file(cmake/ZenRHIConfig.cmake.in "${CMAKE_CURRENT_BINARY_DIR}/ZenRHIConfig.cmake" INSTALL_DESTINATION "${package_dir}")
    write_basic_package_version_file("${CMAKE_CURRENT_BINARY_DIR}/ZenRHIConfigVersion.cmake" COMPATIBILITY SameMinorVersion)
    install(FILES "${CMAKE_CURRENT_BINARY_DIR}/ZenRHIConfig.cmake" "${CMAKE_CURRENT_BINARY_DIR}/ZenRHIConfigVersion.cmake" DESTINATION "${package_dir}")
endif()
