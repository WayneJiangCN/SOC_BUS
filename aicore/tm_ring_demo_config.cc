#include "tm_ring_demo_config.h"

#include <cmath>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <map>
#include <sstream>
#include <stdexcept>
#include <utility>
#include <vector>

namespace tm_ring_demo {
namespace {

std::string trim(const std::string& value)
{
    const size_t begin = value.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) {
        return "";
    }
    const size_t end = value.find_last_not_of(" \t\r\n");
    return value.substr(begin, end - begin + 1);
}

std::string strip_comment(const std::string& line)
{
    bool quoted = false;
    for (size_t i = 0; i < line.size(); ++i) {
        if (line[i] == '"' && (i == 0 || line[i - 1] != '\\')) {
            quoted = !quoted;
        } else if (line[i] == '#' && !quoted) {
            return line.substr(0, i);
        }
    }
    return line;
}

std::string unquote(const std::string& value)
{
    const std::string text = trim(value);
    if (text.size() >= 2 && text.front() == '"' && text.back() == '"') {
        return text.substr(1, text.size() - 2);
    }
    return text;
}

bool parse_u32(const std::string& text, const std::string& key,
               uint32_t* result, std::string* error)
{
    try {
        size_t consumed = 0;
        const unsigned long long value = std::stoull(trim(text), &consumed, 0);
        if (consumed != trim(text).size() ||
            value > std::numeric_limits<uint32_t>::max()) {
            throw std::out_of_range("u32");
        }
        *result = static_cast<uint32_t>(value);
        return true;
    } catch (...) {
        *error = key + " expects a 32-bit unsigned integer, got: " + text;
        return false;
    }
}

bool parse_double(const std::string& text, const std::string& key,
                  double* result, std::string* error)
{
    try {
        size_t consumed = 0;
        const std::string value_text = trim(text);
        const double value = std::stod(value_text, &consumed);
        if (consumed != value_text.size()) {
            throw std::out_of_range("double");
        }
        *result = value;
        return true;
    } catch (...) {
        *error = key + " expects a number, got: " + text;
        return false;
    }
}

bool parse_bool(const std::string& text, const std::string& key,
                bool* result, std::string* error)
{
    const std::string value = trim(text);
    if (value == "true") {
        *result = true;
        return true;
    }
    if (value == "false") {
        *result = false;
        return true;
    }
    *error = key + " expects true or false, got: " + text;
    return false;
}

std::string option_to_key(const std::string& option)
{
    static const std::map<std::string, std::string> options = {
        {"--interleave", "interleave"},
        {"--perf-target", "performance_target_pct"},
        {"--clock-ghz", "clock_ghz"},
        {"--masters", "num_masters"},
        {"--targets", "num_targets"},
        {"--uops", "uops_per_master"},
        {"--cycles", "cycles"},
        {"--interleave-size", "interleave_size"},
        {"--interleave-hash-shift", "interleave_hash_shift"},
        {"--interleave-hash-seed", "interleave_hash_seed"},
        {"--target-width", "target_width_bytes"},
        {"--link-latency", "ring_link_latency"},
        {"--link-width", "ring_link_width_bytes"},
        {"--router-input-depth", "ring_router_input_depth"},
        {"--rsp-phys-lanes", "rsp_phys_lanes"},
        {"--rsp-lane-select", "rsp_lane_select"},
        {"--master-fifo-depth", "master_fifo_depth"},
        {"--target-fifo-depth", "target_fifo_depth"},
        {"--master-rd-osd", "master_rd_osd"},
        {"--master-wr-osd", "master_wr_osd"},
        {"--global-osd", "global_osd"},
        {"--target-rd-osd", "target_rd_osd"},
        {"--target-wr-osd", "target_wr_osd"},
        {"--target-acc-osd", "target_acc_osd"},
    };
    const auto it = options.find(option);
    return it == options.end() ? "" : it->second;
}

}  // namespace

RingDemoConfig make_demo_case(const std::string& name)
{
    RingDemoConfig config;
    if (name == "multi_core") {
        config.name = name;
        return config;
    }
    if (name == "multi_core_backpressure") {
        config.name = name;
        config.num_targets = 2;
        config.cycles = 300000;
        config.master_fifo_depth = 2;
        config.target_fifo_depth = 2;
        return config;
    }
    if (name == "multi_core_rsp_4lane") {
        config.name = name;
        return config;
    }
    throw std::invalid_argument("unknown multi-core ring demo case: " + name);
}

bool apply_demo_value(const std::string& raw_key, const std::string& raw_value,
                      RingDemoConfig* config, std::string* error)
{
    if (config == nullptr || error == nullptr) {
        return false;
    }
    const std::string key = trim(raw_key);
    const std::string value = unquote(raw_value);
    if (key == "name") {
        config->name = value;
        return true;
    }
    if (key == "ddr_config" || key == "pem_config") {
        // pem_config is retained as a compatibility alias.
        config->ddr_config_file = value;
        return true;
    }
    if (key == "interleave") {
        if (value == "none") {
            config->target_interleave = false;
            return true;
        }
        if (value == "linear") {
            config->target_interleave = true;
            config->interleave_type = tm_bus_interleave_type_t::LINEAR;
            return true;
        }
        if (value == "xor") {
            config->target_interleave = true;
            config->interleave_type = tm_bus_interleave_type_t::XOR_HASH;
            return true;
        }
        *error = "interleave expects none, linear or xor, got: " + value;
        return false;
    }
    if (key == "require_performance_target") {
        return parse_bool(value, key, &config->require_performance_target,
                          error);
    }
    if (key == "performance_target_pct") {
        return parse_double(value, key, &config->performance_target_pct, error);
    }
    if (key == "clock_ghz") {
        return parse_double(value, key, &config->clock_ghz, error);
    }
    if (key == "rsp_lane_select") {
        if (value == "target") {
            config->rsp_lane_select = TmRingRspLaneSelect::TARGET;
            return true;
        }
        if (value == "hash") {
            config->rsp_lane_select = TmRingRspLaneSelect::HASH;
            return true;
        }
        if (value == "round_robin") {
            config->rsp_lane_select = TmRingRspLaneSelect::ROUND_ROBIN;
            return true;
        }
        *error = "rsp_lane_select expects target, hash or round_robin, got: " +
                 value;
        return false;
    }

    uint32_t* field = nullptr;
    if (key == "num_masters") field = &config->num_masters;
    else if (key == "num_targets") field = &config->num_targets;
    else if (key == "uops_per_master") field = &config->uops_per_master;
    else if (key == "cycles") field = &config->cycles;
    else if (key == "interleave_size") field = &config->interleave_size;
    else if (key == "interleave_hash_shift") field = &config->interleave_hash_shift;
    else if (key == "interleave_hash_seed") field = &config->interleave_hash_seed;
    else if (key == "target_width_bytes") field = &config->target_width_bytes;
    else if (key == "master_fifo_depth") field = &config->master_fifo_depth;
    else if (key == "target_fifo_depth") field = &config->target_fifo_depth;
    else if (key == "master_rd_osd") field = &config->master_rd_osd;
    else if (key == "master_wr_osd") field = &config->master_wr_osd;
    else if (key == "global_osd") field = &config->global_osd;
    else if (key == "target_rd_osd") field = &config->target_rd_osd;
    else if (key == "target_wr_osd") field = &config->target_wr_osd;
    else if (key == "target_acc_osd") field = &config->target_acc_osd;
    else if (key == "ring_link_latency") field = &config->ring_link_latency;
    else if (key == "ring_link_width_bytes") field = &config->ring_link_width_bytes;
    else if (key == "ring_router_input_depth") field = &config->ring_router_input_depth;
    else if (key == "rsp_phys_lanes") field = &config->rsp_phys_lanes;
    else {
        *error = "unknown configuration key: " + key;
        return false;
    }
    return parse_u32(value, key, field, error);
}

bool load_demo_config(const std::string& file_name,
                      const std::string& case_name,
                      RingDemoConfig* config,
                      std::string* error)
{
    std::ifstream input(file_name);
    if (!input.is_open()) {
        *error = "cannot open ring scenario config: " + file_name;
        return false;
    }

    typedef std::vector<std::pair<std::string, std::string>> Values;
    std::map<std::string, Values> sections;
    std::string section;
    std::string line;
    uint32_t line_number = 0;
    while (std::getline(input, line)) {
        ++line_number;
        const std::string text = trim(strip_comment(line));
        if (text.empty()) continue;
        if (text.front() == '[' && text.back() == ']') {
            section = trim(text.substr(1, text.size() - 2));
            sections[section];
            continue;
        }
        const size_t equal = text.find('=');
        if (section.empty() || equal == std::string::npos) {
            *error = file_name + ":" + std::to_string(line_number) +
                     ": expected [section] or key = value";
            return false;
        }
        sections[section].push_back(
            std::make_pair(trim(text.substr(0, equal)),
                           trim(text.substr(equal + 1))));
    }

    const std::string legacy_case_section = "case." + case_name;
    const std::string ring_case_section = "RING_DEMO.case." + case_name;
    const std::string short_ring_case_section = "RING_DEMO." + case_name;
    const bool has_legacy_ring_config =
        sections.find("default") != sections.end() ||
        sections.find(legacy_case_section) != sections.end();
    const bool has_merged_ring_config =
        sections.find("RING_DEMO") != sections.end() ||
        sections.find(ring_case_section) != sections.end() ||
        sections.find(short_ring_case_section) != sections.end();
    if (!has_legacy_ring_config && !has_merged_ring_config) {
        *error = "missing Ring scenario section in " + file_name +
                 "; add [RING_DEMO] or use the legacy [default]/[" +
                 legacy_case_section + "] format";
        return false;
    }

    try {
        *config = make_demo_case(case_name);
    } catch (const std::exception& ex) {
        *error = ex.what();
        return false;
    }
    if (has_merged_ring_config) {
        // In the merged mode, the same TOML file is used by Ring, DDR and BIU.
        // A ddr_config/pem_config key inside [RING_DEMO] may still override it.
        config->ddr_config_file = file_name;
    }

    std::vector<std::string> ordered_sections;
    ordered_sections.push_back("default");
    ordered_sections.push_back("RING_DEMO");
    ordered_sections.push_back(legacy_case_section);
    ordered_sections.push_back(ring_case_section);
    ordered_sections.push_back(short_ring_case_section);
    for (const auto& current : ordered_sections) {
        const auto found = sections.find(current);
        if (found == sections.end()) continue;
        for (const auto& item : found->second) {
            if (!apply_demo_value(item.first, item.second, config, error)) {
                *error = "[" + current + "] " + *error;
                return false;
            }
        }
    }
    config->name = case_name;
    return validate_config(*config, error);
}

bool parse_option_string(const std::string& options, RingDemoConfig* config,
                         std::string* error)
{
    std::istringstream input(options);
    std::string token;
    while (input >> token) {
        if (token == "--require-perf-target") {
            config->require_performance_target = true;
            continue;
        }
        if (token.rfind("--", 0) != 0) {
            *error = "unexpected option token: " + token;
            return false;
        }
        std::string option = token;
        std::string value;
        const size_t equal = token.find('=');
        if (equal != std::string::npos) {
            option = token.substr(0, equal);
            value = token.substr(equal + 1);
        } else if (!(input >> value)) {
            *error = "missing value for option: " + option;
            return false;
        }
        const std::string key = option_to_key(option);
        if (key.empty()) {
            *error = "unknown option: " + option;
            return false;
        }
        if (!apply_demo_value(key, value, config, error)) return false;
    }
    return true;
}

bool apply_utest_options(RingDemoConfig* config, std::string* error)
{
    const char* options_env = std::getenv("TM_RING_DEMO_OPTIONS");
    if (options_env != nullptr && *options_env != '\0' &&
        !parse_option_string(options_env, config, error)) {
        return false;
    }
    return validate_config(*config, error);
}

bool validate_config(const RingDemoConfig& config, std::string* error)
{
    if (config.num_masters < 2 || config.num_targets == 0 ||
        config.uops_per_master == 0 || config.cycles == 0) {
        *error = "standalone ring scenario requires at least two masters; "
                 "targets, uops and cycles must be non-zero";
        return false;
    }
    if (config.uops_per_master % 2 != 0) {
        *error = "uops_per_master must be even because two reads produce one write";
        return false;
    }
    if (config.interleave_size == 0 || config.target_width_bytes == 0 ||
        config.ring_link_width_bytes == 0 || config.master_fifo_depth == 0 ||
        config.target_fifo_depth == 0 || config.ring_link_latency == 0 ||
        config.ring_router_input_depth == 0 ||
        config.rsp_phys_lanes == 0 ||
        config.master_rd_osd == 0 ||
        config.master_wr_osd == 0 || config.global_osd == 0 ||
        config.target_rd_osd == 0 || config.target_wr_osd == 0 ||
        config.target_acc_osd == 0) {
        *error = "width, FIFO, link latency and all OSD values must be non-zero";
        return false;
    }
    if (!std::isfinite(config.performance_target_pct) ||
        config.performance_target_pct < 0.0 ||
        config.performance_target_pct > 100.0) {
        *error = "performance_target_pct must be between 0 and 100";
        return false;
    }
    if (!std::isfinite(config.clock_ghz) || config.clock_ghz <= 0.0) {
        *error = "clock_ghz must be greater than zero";
        return false;
    }
    if (config.target_interleave &&
        config.interleave_type == tm_bus_interleave_type_t::XOR_HASH &&
        config.interleave_hash_shift >= 64) {
        *error = "interleave_hash_shift must be smaller than 64";
        return false;
    }
    const uint64_t source_bytes =
        static_cast<uint64_t>(config.uops_per_master) * BAND_WIDTH;
    const uint64_t result_bytes =
        static_cast<uint64_t>(config.uops_per_master / 2) * sizeof(uint32_t);
    if (source_bytes > std::numeric_limits<uint32_t>::max() ||
        source_bytes > kMasterAddrStride || result_bytes > kMasterAddrStride) {
        *error = "per-master address range exceeds the configured stride";
        return false;
    }
    const uint64_t last_master = config.num_masters - 1;
    if (kDemoSrcAddr + last_master * kMasterAddrStride + source_bytes >
            kTargetAddressLimit ||
        kDemoDstAddr + last_master * kMasterAddrStride + result_bytes >
            kTargetAddressLimit) {
        *error = "master address range exceeds target address space";
        return false;
    }
    return true;
}

}  // namespace tm_ring_demo
