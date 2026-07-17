#include "tm_ring_demo_test.h"

#include <cstdlib>
#include <gtest/gtest.h>

namespace {

const char* kPemConfig = "../etc/pem_config_cloud.toml";

std::string
selected_case_name()
{
    const char* value = std::getenv("TM_RING_DEMO_CASE");
    return value == nullptr || *value == '\0'
               ? std::string("single_rw")
               : std::string(value);
}

std::string
selected_config_name()
{
    const char* value = std::getenv("TM_RING_DEMO_CONFIG");
    return value == nullptr || *value == '\0' ? std::string(kPemConfig)
                                               : std::string(value);
}

void before_test() {}
void after_test() {}

}  // namespace

template <typename T>
void
utest(const std::string& cfg_file_name)
{
    const std::string case_name = selected_case_name();
    auto test_case = tm_ring_demo::make_demo_case(case_name);
    const std::string result_file = "pem_" + case_name + "_result.txt";
    std::string config_error;
    ASSERT_TRUE(tm_ring_demo::apply_utest_options(
        &test_case, &config_error)) << config_error;

    before_test();
    int status = 0;
    try {
        status = tm_ring_demo::run_demo_to_file<T>(
            cfg_file_name, test_case, result_file);
    } catch (...) {
        after_test();
        throw;
    }
    after_test();

    ASSERT_EQ(status, 0) << "see result file: " << result_file;
}

TEST(pem, utest)
{
    utest<PemTrDemo>(selected_config_name());
}
