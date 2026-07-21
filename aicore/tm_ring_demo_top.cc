#include "tm_ring_demo_test.h"

#include <cstdlib>
#include <gtest/gtest.h>

namespace {

std::string
selected_case_name()
{
    const char* value = std::getenv("TM_RING_DEMO_CASE");
    return value == nullptr || *value == '\0'
               ? std::string("multi_core")
               : std::string(value);
}

std::string
selected_scenario_config()
{
    const char* value = std::getenv("TM_RING_ESL_CONFIG");
    return value == nullptr || *value == '\0'
               ? std::string("config/tm_ring_demo.toml")
               : std::string(value);
}

void before_test() {}
void after_test() {}

}  // namespace

template <typename T>
void utest()
{
    const std::string case_name = selected_case_name();
    tm_ring_demo::RingDemoConfig test_case;
    const std::string result_file = "pem_" + case_name + "_result.txt";
    std::string config_error;
    ASSERT_TRUE(tm_ring_demo::load_demo_config(
        selected_scenario_config(), case_name, &test_case, &config_error))
        << config_error;
    ASSERT_TRUE(tm_ring_demo::apply_utest_options(
        &test_case, &config_error)) << config_error;

    const char* ddr_override = std::getenv("TM_RING_DDR_CONFIG");
    if (ddr_override == nullptr || *ddr_override == '\0') {
        // Legacy GTest environment variable.
        ddr_override = std::getenv("TM_RING_DEMO_CONFIG");
    }
    const std::string ddr_config =
        ddr_override == nullptr || *ddr_override == '\0'
            ? test_case.ddr_config_file
            : std::string(ddr_override);

    before_test();
    int status = 0;
    try {
        status = tm_ring_demo::run_demo_to_file<T>(
            ddr_config, test_case, result_file);
    } catch (...) {
        after_test();
        throw;
    }
    after_test();

    ASSERT_EQ(status, 0) << "see result file: " << result_file;
}

TEST(pem, utest)
{
    utest<PemTrDemo>();
}
