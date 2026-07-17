#include "tm_ring_demo_test.h"

#include <gtest/gtest.h>

namespace {

const char* kPemConfig = "../etc/pem_config_cloud.toml";
const char* kResultFile = "pem_multi_master_multi_target_result.txt";

}  // namespace

template <typename T>
void
utest(const std::string& cfg_file_name)
{
    auto test_case =
        tm_ring_demo::make_demo_case("multi_master_multi_target");

    before_test();
    int status = 0;
    try {
        status = tm_ring_demo::run_demo_to_file<T>(
            cfg_file_name, test_case, kResultFile);
    } catch (...) {
        after_test();
        throw;
    }
    after_test();

    ASSERT_EQ(status, 0) << "see result file: " << kResultFile;
}

TEST(pem, utest)
{
    utest<PemTrDemo>(kPemConfig);
}
