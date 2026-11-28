#include "ZenRHI/DynamicRHI.h"
#include "VulkanIntegrationFixture.h"
#include "ZenRHI/TestSupport/Validation.h"
#include <gtest/gtest.h>

namespace
{
class ValidationListener final : public testing::EmptyTestEventListener
{
    void OnTestStart(const testing::TestInfo&) override
    {
        m_before = zen::test::GetProcessValidationErrors();
    }

    void OnTestEnd(const testing::TestInfo& info) override
    {
        const bool probe = std::string_view(info.test_suite_name()) == "VulkanValidationTest"
                        && std::string_view(info.name()) == "OverlappingBufferClears";
        EXPECT_EQ(zen::test::GetProcessValidationErrors() - m_before, probe ? 1u : 0u);
    }

    uint64_t m_before{};
};
} // namespace

int main(int argc, char** argv)
{
    testing::InitGoogleTest(&argc, argv);
    zen::test::ValidationCounter::Install();
    testing::UnitTest::GetInstance()->listeners().Append(new ValidationListener());
    int status = 0;
    if (argc == 2 && std::string_view(argv[1]) == "--check-device-profile")
    {
        zen::RHIResult<std::vector<zen::RHIAdapterInfo>> adapters = zen::EnumerateRHIAdapters(zen::test::MakeTestDeviceDesc());
        if (!adapters || adapters.GetValue().empty())
        {
            status = 1;
        }
        else
        {
            status = 77;
            for (const zen::RHIAdapterInfo& adapter : adapters.GetValue())
            {
                std::fprintf(stderr, "Vulkan adapter=%s vendor=%u driver=%u api=%u profile=%s\n", adapter.name.c_str(),
                             adapter.vendorID, adapter.driverVersion, adapter.apiVersion,
                             adapter.unsupportedReason.empty() ? "supported" : adapter.unsupportedReason.c_str());
                if (adapter.unsupportedReason.empty())
                {
                    status = 0;
                }
            }
        }
    }
    else
    {
        status = RUN_ALL_TESTS();
    }
    return status;
}
