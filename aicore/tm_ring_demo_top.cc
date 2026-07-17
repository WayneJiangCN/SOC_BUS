#include "tm_ring_demo_test.h"

#include <gtest/gtest.h>

namespace {

const char* kPemConfig = "../etc/pem_config_cloud.toml";

}  // namespace

template <typename T>
void
utest(const std::string& cfg_file_name,
      tm_ring_demo::RingDemoConfig test_case)
{
    std::string error;
    if (!tm_ring_demo::apply_utest_options(&test_case, &error)) {
        throw std::invalid_argument("invalid ring demo config: " + error);
    }

    before_test();
    int status = 0;
    try {
        status = tm_ring_demo::run_demo<T>(cfg_file_name, test_case);
    } catch (...) {
        after_test();
        throw;
    }
    after_test();

    if (status != 0) {
        throw std::runtime_error("ring demo failed: " + test_case.name);
    }
}

template <typename T>
void
utest(const std::string& cfg_file_name, const std::string& case_name)
{
    utest<T>(cfg_file_name, tm_ring_demo::make_demo_case(case_name));
}

template <typename T>
void
utest(const std::string& cfg_file_name)
{
    utest<T>(cfg_file_name, "multi_master");
}

TEST(pem, single_rw)
{
    utest<PemTrDemo>(kPemConfig, "single_rw");
}

// Keep the existing GTest filter as the multi-master compatibility entry.
TEST(pem, utest)
{
    utest<PemTrDemo>(kPemConfig, "multi_master");
}

TEST(pem, multi_target_linear)
{
    utest<PemTrDemo>(kPemConfig, "multi_target_linear");
}

TEST(pem, backpressure)
{
    utest<PemTrDemo>(kPemConfig, "backpressure");
}
