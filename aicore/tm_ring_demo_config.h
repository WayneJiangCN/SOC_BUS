#ifndef _TM_RING_DEMO_CONFIG_H_
#define _TM_RING_DEMO_CONFIG_H_

#include <stdint.h>

#include <string>

#include "pem_trdemo.h"
#include "tm_bus_types.h"

namespace tm_ring_demo {

constexpr uint64_t kDemoSrcAddr = 0x30000000ull;
constexpr uint64_t kDemoDstAddr = 0x40000000ull;
constexpr uint64_t kMasterAddrStride = 0x01000000ull;
constexpr uint64_t kTargetAddressLimit = 0x80000000ull;
constexpr uint32_t kDemoCycles = 200000;
constexpr const char* kDefaultRingScenarioConfig =
    "../etc/pem_config_cloud.toml";
constexpr const char* kDefaultDdrConfig =
    "../etc/pem_config_cloud.toml";

struct RingDemoConfig
{
    std::string name = "multi_core";
    // DDR/PEM runtime TOML: DDR latency/credit and BIU runtime parameters.
    std::string ddr_config_file = kDefaultDdrConfig;
    uint32_t num_masters = 4;
    uint32_t num_targets = 4;
    uint32_t uops_per_master = 256;
    uint32_t cycles = kDemoCycles;

    bool target_interleave = true;
    tm_bus_interleave_type_t interleave_type =
        tm_bus_interleave_type_t::LINEAR;
    uint32_t interleave_size = BAND_WIDTH;
    uint32_t interleave_hash_shift = 6;
    uint32_t interleave_hash_seed = 0;

    uint32_t target_width_bytes = 32;

    uint32_t master_fifo_depth = 16;
    uint32_t target_fifo_depth = 16;

    uint32_t master_rd_osd = 16;
    uint32_t master_wr_osd = 16;
    uint32_t global_osd = 64;
    // The standalone multi-core scenarios always set target OSD explicitly.
    uint32_t target_rd_osd = 16;
    uint32_t target_wr_osd = 16;
    uint32_t target_acc_osd = 32;

    uint32_t ring_link_latency = 1;
    uint32_t ring_link_width_bytes = 16;
    uint32_t ring_router_input_depth = 2;

    double performance_target_pct = 80.0;
    double clock_ghz = 1.0;
    bool require_performance_target = false;
};

RingDemoConfig make_demo_case(const std::string& name);

// Reads Ring scenario keys from either the legacy standalone TOML
// ([default]/[case.<case_name>]) or the merged PEM TOML
// ([RING_DEMO]/[RING_DEMO.case.<case_name>]).
bool load_demo_config(const std::string& file_name,
                      const std::string& case_name,
                      RingDemoConfig* config,
                      std::string* error);

// Applies one TOML-style key/value pair. This is also used by --set.
bool apply_demo_value(const std::string& key, const std::string& value,
                      RingDemoConfig* config, std::string* error);

// Backward-compatible command-line option parser for TM_RING_DEMO_OPTIONS.
bool parse_option_string(const std::string& options, RingDemoConfig* config,
                         std::string* error);
bool apply_utest_options(RingDemoConfig* config, std::string* error);
bool validate_config(const RingDemoConfig& config, std::string* error);

}  // namespace tm_ring_demo

#endif  // _TM_RING_DEMO_CONFIG_H_
