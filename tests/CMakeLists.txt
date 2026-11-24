if(NOT TARGET GTest::gtest_main)
    find_package(GTest CONFIG QUIET)
    if(NOT TARGET GTest::gtest_main)
        set(INSTALL_GTEST OFF)
        set(BUILD_GMOCK OFF)
        set(gtest_force_shared_crt ON CACHE BOOL "Use shared MSVC runtime")
        FetchContent_Declare(googletest URL https://codeload.github.com/google/googletest/zip/refs/tags/v1.18.0
            URL_HASH SHA256=63b9c77751a5b8f492486005f67533fdc58682b67476fcb91650be5958d5195a)
        FetchContent_MakeAvailable(googletest)
    endif()
endif()
add_executable(ZenRHICoreTests RHIThreadTests.cpp RHIGPUTimingTests.cpp RHIGPUFrameTimingTests.cpp
    RHIExecutorTests.cpp)
target_link_libraries(ZenRHICoreTests PRIVATE ZenRHI::TestSupport GTest::gtest_main)
add_test(NAME ZenRHICoreTests COMMAND ZenRHICoreTests)

find_program(GLSL_VALIDATOR glslangValidator REQUIRED HINTS "$ENV{VULKAN_SDK}/Bin")
set(shader_dir "${CMAKE_CURRENT_BINARY_DIR}/shaders")
foreach(shader IN ITEMS
    review_acceleration.comp
    review_runtime_array.comp
    review_reserved_set.comp
    review_duplicate.vert
    review_duplicate.frag
    rdg_access.comp
    rdg_shared.vert
    rdg_shared.frag
    descriptor_storage.comp
    descriptor_array.comp
    descriptor_bindings.comp
    descriptor_uniform.comp
    descriptor_uniform_large.comp
    descriptor_uniform_limit.comp
    descriptor_sampler_limit.comp
    pipeline_specialization.comp
    pipeline.vert
    pipeline.frag
    pipeline_depth.frag
    pipeline_dual_source.frag
    binding_sampler.comp
    binding_recording_array.comp
    binding_single_layer_array.comp
    binding_uniform_array.comp
    binding_bindless.comp
    binding_bindless.frag
    binding_bindless_array.comp
    binding_bindless_cube_array.comp
    binding_push_constants.comp
    recording_uniform.frag
    recording_uniform.comp
    review_matrix.vert
    review_array.vert
    recording_vertex.vert
    async_resources.comp
    binding_texel.comp
    binding_high_set.comp
    binding_required_sets.comp
    binding_validation.comp
    unsupported_tessellation.tesc
    unsupported_tessellation.tese
    rt_shared.rgen
    rt_shared.rmiss
    rt_secondary.rmiss
    rt_shared.rchit
    rt_shared.rahit
    rt_shared.rcall)
    set(input "${CMAKE_CURRENT_SOURCE_DIR}/Shaders/${shader}")
    set(output "${shader_dir}/${shader}.spv")
    set(version spirv1.3)
    if(shader MATCHES "\\.(rgen|rmiss|rchit|rahit|rcall)$")
        set(version spirv1.4)
    endif()
    add_custom_command(OUTPUT "${output}" COMMAND ${CMAKE_COMMAND} -E make_directory "${shader_dir}"
        COMMAND "${GLSL_VALIDATOR}" --target-env ${version} -V "${input}" "-I${ZenRHI_SOURCE_DIR}/include"
        -o "${output}" --depfile "${output}.d" DEPENDS "${input}" DEPFILE "${output}.d" VERBATIM)
    list(APPEND shader_files "${output}")
endforeach()
add_custom_target(ZenRHITestShaders DEPENDS ${shader_files})

add_executable(ZenRHIVulkanTests
    ShaderReflectionTests.cpp
    RayTracingLayoutTests.cpp
    VulkanReviewTests.cpp
    VulkanSynchronizationTests.cpp
    VulkanLifetimeTrackerTests.cpp
    VulkanResourceCreationTests.cpp
    VulkanShaderLayoutTests.cpp
    VulkanVerificationTests.cpp
    VulkanQueueSelectionTests.cpp)
target_link_libraries(ZenRHIVulkanTests PRIVATE GTest::gtest_main)

if(TARGET spirv-reflect AND NOT ZENRHI_INSTALL)
    target_link_libraries(ZenRHIVulkanTests PRIVATE spirv-reflect)
else()
    target_include_directories(ZenRHIVulkanTests PRIVATE "${spirv_reflect_SOURCE_DIR}")
endif()

add_executable(ZenRHIVulkanIntegrationTests
    RHIProgressIntegrationTests.cpp
    VulkanReviewIntegrationTests.cpp
    VulkanPipelineIntegrationTests.cpp
    VulkanDescriptorIntegrationTests.cpp
    VulkanDescriptorLifetimeTests.cpp
    VulkanQueueIntegrationTests.cpp
    VulkanCommandBufferLifetimeTests.cpp
    VulkanGPUTimingIntegrationTests.cpp
    VulkanShaderLayoutIntegrationTests.cpp
    VulkanCommandValidationIntegrationTests.cpp
    VulkanSwapchainIntegrationTests.cpp
    VulkanValidationProbe.cpp
    VulkanProductionFailureTests.cpp
    VulkanBindingIntegrationTests.cpp
    VulkanCapabilityIntegrationTests.cpp
    VulkanAttachmentIntegrationTests.cpp
    VulkanRecordingIntegrationTests.cpp
    VulkanBindlessRetirementIntegrationTests.cpp
    VulkanAliasedQueueIntegrationTests.cpp
    VulkanAsyncComputeIntegrationTests.cpp
    VulkanUploadIntegrationTests.cpp
    VulkanTestMain.cpp)
if(WIN32)
    target_sources(ZenRHIVulkanIntegrationTests PRIVATE NativeTestWindowWin32.cpp)
elseif(APPLE)
    enable_language(OBJCXX)
    target_sources(ZenRHIVulkanIntegrationTests PRIVATE NativeTestWindowMetal.mm)
    target_link_libraries(ZenRHIVulkanIntegrationTests PRIVATE "-framework AppKit" "-framework QuartzCore")
endif()
foreach(target IN ITEMS ZenRHIVulkanTests ZenRHIVulkanIntegrationTests)
    target_link_libraries(${target} PRIVATE ZenRHI::ZenRHI ZenRHI::TestSupport)
    target_compile_definitions(${target} PRIVATE VK_NO_PROTOTYPES RDG_REFLECTION_TEST_PATH="${shader_dir}/")
    target_include_directories(${target} PRIVATE "${ZenRHI_SOURCE_DIR}/src" "${Vulkan_INCLUDE_DIRS}")
    zenrhi_use_backend_headers(${target})
    add_dependencies(${target} ZenRHITestShaders)
    add_test(NAME ${target} COMMAND ${target})
endforeach()
set_tests_properties(ZenRHICoreTests ZenRHIVulkanTests PROPERTIES LABELS unit)
set_tests_properties(ZenRHIVulkanIntegrationTests PROPERTIES LABELS "vulkan;integration" TIMEOUT 300)

option(ZENRHI_RUN_INSTALLED_TESTS "Execute the relocated SDK programs on a native desktop" OFF)
if(ZENRHI_INSTALL AND ZenFoundation_DIR)
    add_test(NAME ZenRHIRelocatedConsumer COMMAND "${CMAKE_COMMAND}"
        "-DRHI_BUILD=${ZenRHI_BINARY_DIR}" "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/package"
        "-DRUN_INSTALLED_TESTS=${ZENRHI_RUN_INSTALLED_TESTS}" "-DSDL3_DIR=${SDL3_DIR}"
        "-DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/cmake/consumer" "-DGENERATOR=${CMAKE_GENERATOR}"
        "-DFOUNDATION_DIR=${ZenFoundation_DIR}" "-DSPDLOG_DIR=${spdlog_DIR}" "-DCONFIG=$<CONFIG>"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/RelocatedConsumer.cmake")
    add_test(NAME ZenRHIEmbeddedPackage COMMAND "${CMAKE_COMMAND}" -S "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embedded"
        -B "${CMAKE_CURRENT_BINARY_DIR}/embedded-package" -G "${CMAKE_GENERATOR}" "-DCMAKE_BUILD_TYPE=$<CONFIG>"
        "-DCMAKE_MAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}" "-DZenFoundation_DIR=${ZenFoundation_DIR}" "-Dspdlog_DIR=${spdlog_DIR}"
        "-DFETCHCONTENT_SOURCE_DIR_SPIRV_REFLECT=${spirv_reflect_SOURCE_DIR}" "-DFETCHCONTENT_SOURCE_DIR_VOLK=${volk_SOURCE_DIR}"
        "-DFETCHCONTENT_SOURCE_DIR_VMA=${vma_SOURCE_DIR}")

    set_tests_properties(ZenRHIRelocatedConsumer ZenRHIEmbeddedPackage PROPERTIES LABELS package)
endif()

find_package(Python3 COMPONENTS Interpreter REQUIRED)
add_test(NAME ZenRHIPublicAPI COMMAND "${Python3_EXECUTABLE}" "${ZenRHI_SOURCE_DIR}/tools/check_public_api.py"
    --foundation-include "$<TARGET_PROPERTY:ZenFoundation::Foundation,INTERFACE_INCLUDE_DIRECTORIES>" COMMAND_EXPAND_LISTS)
add_test(NAME ZenRHIPublicAPIChecker COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_public_api.py")
set_tests_properties(ZenRHIPublicAPI ZenRHIPublicAPIChecker PROPERTIES LABELS policy)

option(ZENRHI_BUILD_REFLECTION_FUZZER "Build the MSVC libFuzzer shader-reflection target" OFF)
if(ZENRHI_BUILD_REFLECTION_FUZZER AND MSVC)
    add_executable(ZenRHIReflectionFuzzer ReflectionFuzzer.cpp)
    target_link_libraries(ZenRHIReflectionFuzzer PRIVATE ZenRHI::Core Vulkan::SPIRV-Tools)
    # Keep coverage instrumentation and libFuzzer's default main out of the SDK and unit-test binaries.
    target_sources(ZenRHIReflectionFuzzer PRIVATE "../src/Core/RHIShaderUtil.cpp")
    if(TARGET spirv-reflect AND NOT ZENRHI_INSTALL)
        target_link_libraries(ZenRHIReflectionFuzzer PRIVATE spirv-reflect)
    else()
        target_sources(ZenRHIReflectionFuzzer PRIVATE "${spirv_reflect_SOURCE_DIR}/spirv_reflect.c")
        target_include_directories(ZenRHIReflectionFuzzer PRIVATE "${spirv_reflect_SOURCE_DIR}")
    endif()
    target_compile_options(ZenRHIReflectionFuzzer PRIVATE /fsanitize=fuzzer)
endif()
