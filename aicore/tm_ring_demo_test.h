#ifndef _TM_RING_DEMO_TEST_H_
#define _TM_RING_DEMO_TEST_H_

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "cfg.h"
#include "pem_biu.h"
#include "pem_trdemo.h"
#include "tm_engine.h"
#include "tm_mem.h"
#include "tm_ring.h"
#include "types.h"

namespace tm_ring_demo {

using namespace tm_engine;

constexpr uint64_t kDemoSrcAddr = 0x30000000ull;
constexpr uint64_t kDemoDstAddr = 0x40000000ull;
constexpr uint64_t kMasterAddrStride = 0x01000000ull;
constexpr uint64_t kTargetAddressLimit = 0x80000000ull;
constexpr uint32_t kDemoCycles = 20000;

struct RingDemoConfig
{
    std::string name = "multi_master_multi_target";
    uint32_t num_masters = 1;
    uint32_t num_targets = 1;
    uint32_t uops_per_master = TOTAL_UOP_COUNT;
    uint32_t cycles = kDemoCycles;

    bool target_interleave = false;
    tm_bus_interleave_type_t interleave_type =
        tm_bus_interleave_type_t::LINEAR;
    uint32_t interleave_size = BAND_WIDTH;
    uint32_t interleave_hash_shift = 6;
    uint32_t interleave_hash_seed = 0;

    uint32_t target_frontend_latency = 1;
    uint32_t target_forward_latency = 0;
    uint32_t target_response_latency = 1;
    uint32_t target_header_latency = 1;
    uint32_t target_width_bytes = 32;
    uint32_t hotspot_threshold = 16;
    uint32_t hotspot_penalty = 0;

    uint32_t master_fifo_depth = 16;
    uint32_t target_fifo_depth = 16;
    uint32_t ring_fifo_depth = 8;

    uint32_t master_rd_osd = 64;
    uint32_t master_wr_osd = 16;
    uint32_t global_osd = 256;
    // Zero means inherit the corresponding limit from the memory config.
    uint32_t target_rd_osd = 0;
    uint32_t target_wr_osd = 0;
    uint32_t target_acc_osd = 0;

    uint32_t ring_link_latency = 1;
    uint32_t ring_link_width_bytes = 16;
    uint32_t ring_req_header_bytes = 16;
    uint32_t ring_rsp_header_bytes = 16;
    uint32_t ring_link_max_inflight = 8;

    double performance_target_pct = 80.0;
    double clock_ghz = 1.0;
    bool require_performance_target = false;
};

inline RingDemoConfig
make_demo_case(const std::string& name)
{
    RingDemoConfig tc;
    tc.name = name;
    tc.num_masters = 4;
    tc.num_targets = 4;
    tc.uops_per_master = 256;
    tc.cycles = 50000;
    tc.target_interleave = true;
    return tc;
}

inline bool
parse_u32(const std::string& text, const std::string& option,
          uint32_t* result, std::string* error)
{
    try {
        size_t consumed = 0;
        const unsigned long long value = std::stoull(text, &consumed, 0);
        if (consumed != text.size() ||
            value > std::numeric_limits<uint32_t>::max()) {
            throw std::out_of_range("u32");
        }
        *result = static_cast<uint32_t>(value);
        return true;
    } catch (...) {
        *error = option + " expects a 32-bit unsigned integer, got: " + text;
        return false;
    }
}

inline bool
parse_double(const std::string& text, const std::string& option,
             double* result, std::string* error)
{
    try {
        size_t consumed = 0;
        const double value = std::stod(text, &consumed);
        if (consumed != text.size()) {
            throw std::out_of_range("double");
        }
        *result = value;
        return true;
    } catch (...) {
        *error = option + " expects a number, got: " + text;
        return false;
    }
}

inline bool
apply_option(const std::string& name, const std::string& value,
             RingDemoConfig* tc, std::string* error)
{
    if (name == "--interleave") {
        if (value == "none") {
            tc->target_interleave = false;
            return true;
        }
        if (value == "linear") {
            tc->target_interleave = true;
            tc->interleave_type = tm_bus_interleave_type_t::LINEAR;
            return true;
        }
        if (value == "xor") {
            tc->target_interleave = true;
            tc->interleave_type = tm_bus_interleave_type_t::XOR_HASH;
            return true;
        }
        *error = "--interleave expects none, linear or xor, got: " + value;
        return false;
    }
    if (name == "--perf-target") {
        return parse_double(value, name, &tc->performance_target_pct, error);
    }
    if (name == "--clock-ghz") {
        return parse_double(value, name, &tc->clock_ghz, error);
    }

    uint32_t* field = nullptr;
    if (name == "--masters") {
        field = &tc->num_masters;
    } else if (name == "--targets") {
        field = &tc->num_targets;
    } else if (name == "--uops") {
        field = &tc->uops_per_master;
    } else if (name == "--cycles") {
        field = &tc->cycles;
    } else if (name == "--interleave-size") {
        field = &tc->interleave_size;
    } else if (name == "--interleave-hash-shift") {
        field = &tc->interleave_hash_shift;
    } else if (name == "--interleave-hash-seed") {
        field = &tc->interleave_hash_seed;
    } else if (name == "--target-frontend-latency") {
        field = &tc->target_frontend_latency;
    } else if (name == "--target-forward-latency") {
        field = &tc->target_forward_latency;
    } else if (name == "--target-response-latency") {
        field = &tc->target_response_latency;
    } else if (name == "--target-header-latency") {
        field = &tc->target_header_latency;
    } else if (name == "--target-width") {
        field = &tc->target_width_bytes;
    } else if (name == "--hotspot-threshold") {
        field = &tc->hotspot_threshold;
    } else if (name == "--hotspot-penalty") {
        field = &tc->hotspot_penalty;
    } else if (name == "--link-latency") {
        field = &tc->ring_link_latency;
    } else if (name == "--link-width") {
        field = &tc->ring_link_width_bytes;
    } else if (name == "--link-max-inflight") {
        field = &tc->ring_link_max_inflight;
    } else if (name == "--master-fifo-depth") {
        field = &tc->master_fifo_depth;
    } else if (name == "--target-fifo-depth") {
        field = &tc->target_fifo_depth;
    } else if (name == "--ring-fifo-depth") {
        field = &tc->ring_fifo_depth;
    } else if (name == "--master-rd-osd") {
        field = &tc->master_rd_osd;
    } else if (name == "--master-wr-osd") {
        field = &tc->master_wr_osd;
    } else if (name == "--global-osd") {
        field = &tc->global_osd;
    } else if (name == "--target-rd-osd") {
        field = &tc->target_rd_osd;
    } else if (name == "--target-wr-osd") {
        field = &tc->target_wr_osd;
    } else if (name == "--target-acc-osd") {
        field = &tc->target_acc_osd;
    } else {
        *error = "unknown option: " + name;
        return false;
    }
    return parse_u32(value, name, field, error);
}

inline bool
validate_config(const RingDemoConfig& tc, std::string* error);

inline bool
parse_option_string(const std::string& options, RingDemoConfig* tc,
                    std::string* error)
{
    std::istringstream input(options);
    std::string token;
    while (input >> token) {
        if (token == "--require-perf-target") {
            tc->require_performance_target = true;
            continue;
        }
        if (token.rfind("--", 0) != 0) {
            *error = "unexpected option token: " + token;
            return false;
        }

        std::string name = token;
        std::string value;
        const size_t equal = token.find('=');
        if (equal != std::string::npos) {
            name = token.substr(0, equal);
            value = token.substr(equal + 1);
        } else if (!(input >> value)) {
            *error = "missing value for option: " + name;
            return false;
        }
        if (!apply_option(name, value, tc, error)) {
            return false;
        }
    }
    return true;
}

inline bool
apply_utest_options(RingDemoConfig* tc, std::string* error)
{
    const char* options_env = std::getenv("TM_RING_DEMO_OPTIONS");
    if (options_env != nullptr && *options_env != '\0' &&
        !parse_option_string(options_env, tc, error)) {
        return false;
    }
    return validate_config(*tc, error);
}

inline bool
validate_config(const RingDemoConfig& tc, std::string* error)
{
    if (tc.num_masters == 0 || tc.num_targets == 0 ||
        tc.uops_per_master == 0 || tc.cycles == 0) {
        *error = "masters, targets, uops and cycles must be non-zero";
        return false;
    }
    if (tc.uops_per_master % 2 != 0) {
        *error = "uops must be even because two reads produce one write";
        return false;
    }
    if (tc.interleave_size == 0 || tc.target_width_bytes == 0 ||
        tc.ring_link_width_bytes == 0 || tc.master_fifo_depth == 0 ||
        tc.target_fifo_depth == 0 || tc.ring_fifo_depth == 0 ||
        tc.ring_link_max_inflight == 0 || tc.master_rd_osd == 0 ||
        tc.master_wr_osd == 0 || tc.global_osd == 0) {
        *error = "width, FIFO, link inflight and master/global OSD values "
                 "must be non-zero";
        return false;
    }
    if (!std::isfinite(tc.performance_target_pct) ||
        tc.performance_target_pct < 0.0 ||
        tc.performance_target_pct > 100.0) {
        *error = "performance target must be between 0 and 100";
        return false;
    }
    if (!std::isfinite(tc.clock_ghz) || tc.clock_ghz <= 0.0) {
        *error = "clock frequency must be greater than zero";
        return false;
    }
    if (tc.target_interleave &&
        tc.interleave_type == tm_bus_interleave_type_t::XOR_HASH &&
        tc.interleave_hash_shift >= 64) {
        *error = "interleave hash shift must be smaller than 64";
        return false;
    }
    const uint64_t source_bytes =
        static_cast<uint64_t>(tc.uops_per_master) * BAND_WIDTH;
    const uint64_t result_bytes =
        static_cast<uint64_t>(tc.uops_per_master / 2) * sizeof(uint32_t);
    if (source_bytes > std::numeric_limits<uint32_t>::max() ||
        source_bytes > kMasterAddrStride || result_bytes > kMasterAddrStride) {
        *error = "per-master address range exceeds the configured stride";
        return false;
    }
    const uint64_t last_master = tc.num_masters - 1;
    if (kDemoSrcAddr + last_master * kMasterAddrStride + source_bytes >
            kTargetAddressLimit ||
        kDemoDstAddr + last_master * kMasterAddrStride + result_bytes >
            kTargetAddressLimit) {
        *error = "master address range exceeds target address space";
        return false;
    }
    return true;
}

inline uint32_t
configured_or(uint32_t configured, uint32_t fallback)
{
    return configured == 0 ? fallback : configured;
}

inline p_tm_ring_target_cfg_t
make_ddr_target_cfg(const p_tm_mem_cfg_t& ddr_cfg,
                    const RingDemoConfig& tc, uint32_t target_id)
{
    auto target = std::make_shared<tm_ring_target_cfg_t>();
    target->name = "ddr" + std::to_string(target_id);
    target->addr_begin = 0;
    target->size = kTargetAddressLimit;
    target->is_default = !tc.target_interleave && target_id == 0;
    if (tc.target_interleave) {
        target->interleave_type = tc.interleave_type;
        target->interleave_size = tc.interleave_size;
        target->interleave_num = tc.num_targets;
        target->interleave_idx = target_id;
        target->interleave_hash_shift = tc.interleave_hash_shift;
        target->interleave_hash_seed = tc.interleave_hash_seed;
    }

    target->frontend_latency = tc.target_frontend_latency;
    target->forward_latency = tc.target_forward_latency;
    target->response_latency = tc.target_response_latency;
    target->header_latency = tc.target_header_latency;
    target->width = tc.target_width_bytes;
    target->hotspot_threshold = tc.hotspot_threshold;
    target->hotspot_penalty = tc.hotspot_penalty;
    target->rd_req_fifo_depth = tc.target_fifo_depth;
    target->wr_req_fifo_depth = tc.target_fifo_depth;
    target->wr_dat_fifo_depth = tc.target_fifo_depth;

    target->rd_slot_credit_max =
        configured_or(tc.target_rd_osd, ddr_cfg->ddr->max_rd_crdt);
    target->wr_slot_credit_max =
        configured_or(tc.target_wr_osd, ddr_cfg->ddr->max_wr_crdt);
    target->acc_slot_credit_max =
        configured_or(tc.target_acc_osd, ddr_cfg->ddr->max_acc_crdt);
    // Slot credits model target OSD; bandwidth tokens remain byte credits.
    target->acc_bw_token_max = ddr_cfg->ddr->max_acc_crdt;
    target->rd_bw_token_max = ddr_cfg->ddr->max_rd_crdt;
    target->wr_bw_token_max = ddr_cfg->ddr->max_wr_crdt;
    target->token_update_period = ddr_cfg->ddr->crdt_update_period;
    target->acc_bw_token_update = ddr_cfg->ddr->acc_crdt_update;
    target->rd_bw_token_update = ddr_cfg->ddr->rd_crdt_update;
    target->wr_bw_token_update = ddr_cfg->ddr->wr_crdt_update;
    return target;
}

inline p_tm_ring_cfg_t
make_demo_ring_cfg(const p_tm_mem_cfg_t& ddr_cfg,
                   const RingDemoConfig& tc)
{
    auto cfg = tm_make_ring_cfg();
    cfg->name = "demo_ring_" + tc.name;
    cfg->num_masters = tc.num_masters;
    cfg->num_targets = tc.num_targets;
    cfg->rd_rsp_port_num = 2;
    cfg->master_inf_depth = 2;
    cfg->target_inf_depth = 2;

    cfg->master_rd_cmd_fifo_depth = tc.master_fifo_depth;
    cfg->master_wr_cmd_fifo_depth = tc.master_fifo_depth;
    cfg->master_wr_dat_fifo_depth = tc.master_fifo_depth;
    cfg->master_wr_rsp_fifo_depth = tc.master_fifo_depth;
    cfg->master_rd_osd = tc.master_rd_osd;
    cfg->master_wr_osd = tc.master_wr_osd;
    cfg->global_osd = tc.global_osd;

    cfg->ring_req_fifo_depth = tc.ring_fifo_depth;
    cfg->ring_rsp_fifo_depth = tc.ring_fifo_depth;
    cfg->ring_link_latency = tc.ring_link_latency;
    cfg->ring_link_width_bytes = tc.ring_link_width_bytes;
    cfg->ring_req_header_bytes = tc.ring_req_header_bytes;
    cfg->ring_rsp_header_bytes = tc.ring_rsp_header_bytes;
    cfg->ring_req_link_max_inflight = tc.ring_link_max_inflight;
    cfg->ring_rsp_link_max_inflight = tc.ring_link_max_inflight;

    for (uint32_t target = 0; target < tc.num_targets; ++target) {
        cfg->targets.push_back(make_ddr_target_cfg(ddr_cfg, tc, target));
    }
    return cfg;
}

inline bool
preload_demo_data(const std::vector<p_tm_mem_t>& targets, uint64_t addr,
                  uint32_t bytes)
{
    std::vector<uint8_t> data(bytes, 1);
    bool success = true;
    for (auto& target : targets) {
        success = target->pv_write(addr, bytes, data.data(), false) && success;
    }
    return success;
}

inline p_isa_t
make_demo_instr(uint64_t src_addr, uint64_t dst_addr, uint32_t uops)
{
    auto instr = std::make_shared<isa_t>();
    instr->get_binfo()->name_ = isa_name_t::ZADD;
    instr->start_addr_ = src_addr;
    instr->end_addr_ = dst_addr;
    instr->number_size_ = uops;
    return instr;
}

inline uint32_t
target_for_address(const RingDemoConfig& tc, uint64_t addr)
{
    if (!tc.target_interleave) {
        return 0;
    }
    const uint64_t stripe = addr / tc.interleave_size;
    if (tc.interleave_type == tm_bus_interleave_type_t::XOR_HASH) {
        const uint64_t hashed = stripe ^
                                (stripe >> tc.interleave_hash_shift) ^
                                tc.interleave_hash_seed;
        return static_cast<uint32_t>(hashed % tc.num_targets);
    }
    return static_cast<uint32_t>(stripe % tc.num_targets);
}

inline const char*
interleave_name(const RingDemoConfig& tc)
{
    if (!tc.target_interleave) {
        return "none";
    }
    return tc.interleave_type == tm_bus_interleave_type_t::XOR_HASH
               ? "xor"
               : "linear";
}

inline uint32_t
expected_demo_result()
{
    constexpr uint32_t kPreloadWord = 0x01010101u;
    const uint64_t words_per_pair = 2ull * BAND_WIDTH / sizeof(uint32_t);
    return static_cast<uint32_t>(words_per_pair * kPreloadWord);
}

struct MemoryCheck
{
    uint64_t checked = 0;
    uint64_t mismatches = 0;
    uint64_t read_failures = 0;
};

inline MemoryCheck
verify_demo_memory(const std::vector<p_tm_mem_t>& targets,
                   const RingDemoConfig& tc,
                   std::vector<std::string>* failures)
{
    MemoryCheck result;
    const uint32_t expected = expected_demo_result();
    const uint32_t pairs_per_master = tc.uops_per_master / 2;
    uint32_t detailed_errors = 0;
    constexpr uint32_t kMaxDetailedErrors = 8;

    for (uint32_t master = 0; master < tc.num_masters; ++master) {
        const uint64_t dst_base =
            kDemoDstAddr + master * kMasterAddrStride;
        for (uint32_t pair = 0; pair < pairs_per_master; ++pair) {
            const uint64_t addr = dst_base + pair * sizeof(uint32_t);
            const uint32_t target_id = target_for_address(tc, addr);
            uint32_t actual = 0;
            ++result.checked;
            if (!targets[target_id]->pv_read(
                    addr, sizeof(actual),
                    reinterpret_cast<uint8_t*>(&actual))) {
                ++result.read_failures;
                if (detailed_errors++ < kMaxDetailedErrors) {
                    std::ostringstream os;
                    os << "memory read failed: master=" << master
                       << " pair=" << pair << " target=" << target_id
                       << " addr=0x" << std::hex << addr;
                    failures->push_back(os.str());
                }
                continue;
            }
            if (actual != expected) {
                ++result.mismatches;
                if (detailed_errors++ < kMaxDetailedErrors) {
                    std::ostringstream os;
                    os << "memory mismatch: master=" << master
                       << " pair=" << pair << " target=" << target_id
                       << " addr=0x" << std::hex << addr
                       << " expected=0x" << expected
                       << " actual=0x" << actual;
                    failures->push_back(os.str());
                }
            }
        }
    }
    return result;
}

inline void
check_count(std::vector<std::string>* failures, uint32_t master,
            const char* name, uint64_t actual, uint64_t expected)
{
    if (actual == expected) {
        return;
    }
    std::ostringstream os;
    os << "master " << master << " " << name << ": expected=" << expected
       << " actual=" << actual;
    failures->push_back(os.str());
}

inline uint64_t
latency_min_or_zero(uint64_t count, uint64_t value)
{
    return count == 0 ? 0 : value;
}

template <typename DemoT>
inline int
run_demo(const std::string& cfg_file_name, const RingDemoConfig& test_case)
{
    tm_init();
    auto clk = tm_make_clk();

    auto demo_cfg = std::make_shared<cfg::Cfg>();
    demo_cfg->read_cfg_file(cfg_file_name);
    auto ddr_cfg = tm_make_mem_cfg(std::string("ddr"), demo_cfg);

    std::vector<p_tm_mem_t> targets;
    for (uint32_t target = 0; target < test_case.num_targets; ++target) {
        auto mem_cfg = tm_make_mem_cfg(
            "ddr" + std::to_string(target), demo_cfg);
        targets.push_back(tm_make_mem(clk, mem_cfg));
    }

    std::vector<std::string> failures;
    for (uint32_t master = 0; master < test_case.num_masters; ++master) {
        const uint64_t src_addr =
            kDemoSrcAddr + master * kMasterAddrStride;
        const uint32_t bytes = test_case.uops_per_master * BAND_WIDTH;
        if (!preload_demo_data(targets, src_addr, bytes)) {
            std::ostringstream os;
            os << "source preload failed for master " << master;
            failures.push_back(os.str());
        }
    }

    auto ring_cfg = make_demo_ring_cfg(ddr_cfg, test_case);
    auto ring = tm_make_ring(clk, ring_cfg);
    ring->build();

    std::vector<p_pem_biu_t> bius;
    std::vector<std::shared_ptr<DemoT>> demos;
    for (uint32_t master = 0; master < test_case.num_masters; ++master) {
        auto biu = std::make_shared<pem_biu_t>(
            "biu" + std::to_string(master), clk,
            demo_cfg->get_cfg_tab("BIU"));
        biu->core_id_ = master;
        biu->build();
        ring->attach_master(master, biu);
        bius.push_back(biu);

        const uint64_t src_addr =
            kDemoSrcAddr + master * kMasterAddrStride;
        const uint64_t dst_addr =
            kDemoDstAddr + master * kMasterAddrStride;
        auto demo = std::make_shared<DemoT>(
            "pem_trdemo" + std::to_string(master), clk);
        demo->configure_traffic(
            src_addr, dst_addr, test_case.uops_per_master);
        demo->attach(biu);
        demo->build();
        demos.push_back(demo);
    }

    for (uint32_t target = 0; target < test_case.num_targets; ++target) {
        ring->attach_target(target, targets[target]);
    }

    ring->reset();
    for (auto& biu : bius) {
        biu->reset();
    }
    for (auto& demo : demos) {
        demo->reset();
    }

    for (uint32_t master = 0; master < test_case.num_masters; ++master) {
        const uint64_t src_addr =
            kDemoSrcAddr + master * kMasterAddrStride;
        const uint64_t dst_addr =
            kDemoDstAddr + master * kMasterAddrStride;
        demos[master]->instr_que_->push_back(make_demo_instr(
            src_addr, dst_addr, test_case.uops_per_master));
    }

    tm_start(test_case.cycles);
    stats::stat->dump();

    const bool ring_idle = ring->idle();
    bool demo_idle = true;
    bool biu_idle = true;
    bool target_idle = true;
    for (auto& demo : demos) {
        demo_idle = demo_idle && demo->idle();
    }
    for (auto& biu : bius) {
        biu_idle = biu_idle && biu->idle();
    }
    for (auto& target : targets) {
        target_idle = target_idle && target->idle();
    }
    if (!ring_idle) {
        failures.push_back("ring is not idle at cycle limit");
    }
    if (!demo_idle) {
        failures.push_back("traffic generator is not idle at cycle limit");
    }
    if (!biu_idle) {
        failures.push_back("BIU is not idle at cycle limit");
    }
    if (!target_idle) {
        failures.push_back("target memory is not idle at cycle limit");
    }

    const uint64_t expected_reads = test_case.uops_per_master;
    const uint64_t expected_writes = test_case.uops_per_master / 2;
    uint64_t total_reads = 0;
    uint64_t total_read_responses = 0;
    uint64_t total_pairs = 0;
    uint64_t total_writes = 0;
    uint64_t total_write_responses = 0;
    uint64_t total_read_bytes = 0;
    uint64_t total_write_bytes = 0;
    uint64_t total_read_stalls = 0;
    uint64_t total_read_response_stalls = 0;
    uint64_t total_write_stalls = 0;
    uint64_t total_write_buffer_stalls = 0;
    uint64_t total_protocol_errors = 0;
    uint64_t total_read_latency = 0;
    uint64_t total_write_latency = 0;
    uint64_t global_first_cycle = std::numeric_limits<uint64_t>::max();
    uint64_t global_last_read_cycle = 0;
    uint64_t global_last_cycle = 0;
    std::vector<double> master_rates;

    std::cout << "TEST_CONFIG case=" << test_case.name
              << " masters=" << test_case.num_masters
              << " targets=" << test_case.num_targets
              << " uops_per_master=" << test_case.uops_per_master
              << " cycle_limit=" << test_case.cycles
              << " clock_ghz=" << test_case.clock_ghz << std::endl;
    std::cout << "TEST_BUS_CONFIG interleave="
              << interleave_name(test_case)
              << " interleave_size=" << test_case.interleave_size
              << " link_latency=" << test_case.ring_link_latency
              << " link_width=" << test_case.ring_link_width_bytes
              << " link_max_inflight="
              << test_case.ring_link_max_inflight
              << " ring_fifo_depth=" << test_case.ring_fifo_depth
              << " target_width=" << test_case.target_width_bytes
              << " target_latency="
              << (test_case.target_frontend_latency +
                  test_case.target_forward_latency +
                  test_case.target_response_latency +
                  test_case.target_header_latency)
              << " ddr_read_latency=" << ddr_cfg->ddr->min_rd_lat
              << " ddr_write_latency=" << ddr_cfg->ddr->min_wr_lat
              << " master_rd_osd=" << test_case.master_rd_osd
              << " master_wr_osd=" << test_case.master_wr_osd
              << " global_osd=" << test_case.global_osd
              << " target_rd_osd="
              << configured_or(test_case.target_rd_osd,
                               ddr_cfg->ddr->max_rd_crdt)
              << " target_wr_osd="
              << configured_or(test_case.target_wr_osd,
                               ddr_cfg->ddr->max_wr_crdt)
              << " target_acc_osd="
              << configured_or(test_case.target_acc_osd,
                               ddr_cfg->ddr->max_acc_crdt)
              << " hotspot_threshold=" << test_case.hotspot_threshold
              << " hotspot_penalty=" << test_case.hotspot_penalty
              << std::endl;

    for (uint32_t master = 0; master < test_case.num_masters; ++master) {
        const auto& stat = demos[master]->traffic_stats();
        check_count(&failures, master, "read_requests", stat.read_requests,
                    expected_reads);
        check_count(&failures, master, "read_responses", stat.read_responses,
                    expected_reads);
        check_count(&failures, master, "completed_pairs",
                    stat.completed_pairs, expected_writes);
        check_count(&failures, master, "write_requests", stat.write_requests,
                    expected_writes);
        check_count(&failures, master, "write_responses",
                    stat.write_responses, expected_writes);
        if (stat.protocol_errors != 0) {
            std::ostringstream os;
            os << "master " << master
               << " protocol_errors=" << stat.protocol_errors;
            failures.push_back(os.str());
        }

        uint64_t active_cycles = 0;
        if (stat.has_first_read_cycle) {
            global_first_cycle =
                std::min(global_first_cycle, stat.first_read_cycle);
        }
        if (stat.has_last_read_response_cycle) {
            global_last_read_cycle =
                std::max(global_last_read_cycle,
                         stat.last_read_response_cycle);
        }
        if (stat.has_first_read_cycle && stat.has_last_write_response_cycle &&
            stat.last_write_response_cycle >= stat.first_read_cycle) {
            active_cycles = stat.last_write_response_cycle -
                            stat.first_read_cycle + 1;
            global_last_cycle =
                std::max(global_last_cycle, stat.last_write_response_cycle);
        }
        const uint64_t payload_bytes =
            stat.read_responses * BAND_WIDTH +
            stat.write_responses * sizeof(uint32_t);
        const double payload_bpc =
            active_cycles == 0
                ? 0.0
                : static_cast<double>(payload_bytes) / active_cycles;
        master_rates.push_back(payload_bpc);

        const double read_avg =
            stat.read_responses == 0
                ? 0.0
                : static_cast<double>(stat.read_latency_sum) /
                      stat.read_responses;
        const double write_avg =
            stat.write_responses == 0
                ? 0.0
                : static_cast<double>(stat.write_latency_sum) /
                      stat.write_responses;
        std::cout << std::fixed << std::setprecision(3)
                  << "MASTER_PERF master=" << master
                  << " active_cycles=" << active_cycles
                  << " reads=" << stat.read_responses
                  << " writes=" << stat.write_responses
                  << " payload_bytes_per_cycle=" << payload_bpc
                  << " read_latency_avg=" << read_avg
                  << " read_latency_min="
                  << latency_min_or_zero(stat.read_responses,
                                         stat.read_latency_min)
                  << " read_latency_max=" << stat.read_latency_max
                  << " write_latency_avg=" << write_avg
                  << " write_latency_min="
                  << latency_min_or_zero(stat.write_responses,
                                         stat.write_latency_min)
                  << " write_latency_max=" << stat.write_latency_max
                  << " endpoint_stalls="
                  << (stat.read_send_stalls + stat.read_response_stalls +
                      stat.write_send_stalls + stat.write_buffer_stalls)
                  << std::endl;

        total_reads += stat.read_requests;
        total_read_responses += stat.read_responses;
        total_pairs += stat.completed_pairs;
        total_writes += stat.write_requests;
        total_write_responses += stat.write_responses;
        // Throughput is based on completed responses. Counting issued bytes
        // makes an incomplete/deadlocked run look faster than the link peak.
        total_read_bytes += stat.read_responses * BAND_WIDTH;
        total_write_bytes += stat.write_responses * sizeof(uint32_t);
        total_read_stalls += stat.read_send_stalls;
        total_read_response_stalls += stat.read_response_stalls;
        total_write_stalls += stat.write_send_stalls;
        total_write_buffer_stalls += stat.write_buffer_stalls;
        total_protocol_errors += stat.protocol_errors;
        total_read_latency += stat.read_latency_sum;
        total_write_latency += stat.write_latency_sum;
    }

    const MemoryCheck memory =
        verify_demo_memory(targets, test_case, &failures);
    if (memory.read_failures != 0 || memory.mismatches != 0) {
        std::ostringstream os;
        os << "memory verification failed: read_failures="
           << memory.read_failures << " mismatches=" << memory.mismatches;
        failures.push_back(os.str());
    }

    const uint64_t completion_cycles =
        global_first_cycle == std::numeric_limits<uint64_t>::max() ||
                global_last_cycle < global_first_cycle
            ? 0
            : global_last_cycle - global_first_cycle + 1;
    const uint64_t read_completion_cycles =
        global_first_cycle == std::numeric_limits<uint64_t>::max() ||
                global_last_read_cycle < global_first_cycle
            ? 0
            : global_last_read_cycle - global_first_cycle + 1;
    const double read_bpc =
        read_completion_cycles == 0
            ? 0.0
            : static_cast<double>(total_read_bytes) /
                  read_completion_cycles;
    const double write_bpc =
        completion_cycles == 0
            ? 0.0
            : static_cast<double>(total_write_bytes) / completion_cycles;
    const double total_bpc =
        completion_cycles == 0
            ? 0.0
            : static_cast<double>(total_read_bytes + total_write_bytes) /
                  completion_cycles;
    const double pair_ops_per_cycle =
        completion_cycles == 0
            ? 0.0
            : static_cast<double>(total_pairs) / completion_cycles;
    const double read_latency_avg =
        total_read_responses == 0
            ? 0.0
            : static_cast<double>(total_read_latency) /
                  total_read_responses;
    const double write_latency_avg =
        total_write_responses == 0
            ? 0.0
            : static_cast<double>(total_write_latency) /
                  total_write_responses;

    double rate_sum = 0.0;
    double rate_square_sum = 0.0;
    for (double rate : master_rates) {
        rate_sum += rate;
        rate_square_sum += rate * rate;
    }
    const double fairness =
        rate_square_sum == 0.0
            ? 0.0
            : rate_sum * rate_sum /
                  (master_rates.size() * rate_square_sum);
    const uint64_t endpoint_stalls =
        total_read_stalls + total_read_response_stalls +
        total_write_stalls + total_write_buffer_stalls;
    const uint64_t global_osd_stalls = ring->global_osd_stalls();
    const uint64_t target_slot_stalls = ring->target_slot_stalls();
    const uint64_t bandwidth_token_stalls =
        ring->bandwidth_token_stalls();
    const uint64_t ring_link_stalls = ring->ring_link_stalls();
    const uint64_t fabric_stalls = global_osd_stalls + target_slot_stalls +
                                   bandwidth_token_stalls + ring_link_stalls;
    const uint64_t all_stalls = endpoint_stalls + fabric_stalls;
    const char* dominant_bottleneck = "none";
    uint64_t dominant_stalls = 0;
    if (global_osd_stalls > dominant_stalls) {
        dominant_stalls = global_osd_stalls;
        dominant_bottleneck = "global_osd";
    }
    if (target_slot_stalls > dominant_stalls) {
        dominant_stalls = target_slot_stalls;
        dominant_bottleneck = "target_slot";
    }
    if (bandwidth_token_stalls > dominant_stalls) {
        dominant_stalls = bandwidth_token_stalls;
        dominant_bottleneck = "bandwidth_token";
    }
    if (ring_link_stalls > dominant_stalls) {
        dominant_stalls = ring_link_stalls;
        dominant_bottleneck = "ring_link";
    }

    const uint32_t parallel_paths =
        std::min(test_case.num_masters, test_case.num_targets);
    const uint32_t path_width =
        std::min(test_case.ring_link_width_bytes,
                 test_case.target_width_bytes);
    const double estimated_peak_bpc =
        static_cast<double>(parallel_paths) * path_width;
    const double utilization_pct =
        estimated_peak_bpc == 0.0
            ? 0.0
            : 100.0 * read_bpc / estimated_peak_bpc;
    const uint64_t expected_total_reads =
        expected_reads * test_case.num_masters;
    const uint64_t expected_total_writes =
        expected_writes * test_case.num_masters;
    const bool measurement_valid =
        total_reads == expected_total_reads &&
        total_read_responses == expected_total_reads &&
        total_pairs == expected_total_writes &&
        total_writes == expected_total_writes &&
        total_write_responses == expected_total_writes &&
        total_protocol_errors == 0 && ring_idle && demo_idle && biu_idle &&
        target_idle;
    const bool performance_target_met =
        measurement_valid &&
        utilization_pct >= test_case.performance_target_pct;
    if (test_case.require_performance_target && !performance_target_met) {
        std::ostringstream os;
        os << "performance target missed: target="
           << test_case.performance_target_pct
           << "% actual=" << utilization_pct << "%";
        failures.push_back(os.str());
    }

    std::cout << std::fixed << std::setprecision(3)
              << "TEST_COUNTS read_requests=" << total_reads
              << " read_responses=" << total_read_responses
              << " completed_pairs=" << total_pairs
              << " write_requests=" << total_writes
              << " write_responses=" << total_write_responses
              << " protocol_errors=" << total_protocol_errors << std::endl;
    std::cout << "TEST_MEMORY checked=" << memory.checked
              << " mismatches=" << memory.mismatches
              << " read_failures=" << memory.read_failures
              << " expected_value=0x" << std::hex << expected_demo_result()
              << std::dec << std::endl;
    std::cout << "TEST_PERF completion_cycles=" << completion_cycles
              << " read_completion_cycles=" << read_completion_cycles
              << " read_payload_bytes_per_cycle=" << read_bpc
              << " write_payload_bytes_per_cycle=" << write_bpc
              << " total_payload_bytes_per_cycle=" << total_bpc
              << " read_bandwidth_GBps="
              << (read_bpc * test_case.clock_ghz)
              << " total_payload_bandwidth_GBps="
              << (total_bpc * test_case.clock_ghz)
              << " pair_ops_per_cycle=" << pair_ops_per_cycle << std::endl;
    std::cout << "TEST_UTILIZATION estimated_peak_bytes_per_cycle="
              << estimated_peak_bpc
              << " utilization_pct=" << utilization_pct
              << " target_pct=" << test_case.performance_target_pct
              << " measurement_valid="
              << (measurement_valid ? "yes" : "no")
              << " target_met="
              << (performance_target_met ? "yes" : "no") << std::endl;
    std::cout << "TEST_LATENCY read_avg_cycles=" << read_latency_avg
              << " write_avg_cycles=" << write_latency_avg << std::endl;
    std::cout << "TEST_STALLS read_send=" << total_read_stalls
              << " read_response=" << total_read_response_stalls
              << " write_send=" << total_write_stalls
              << " write_buffer=" << total_write_buffer_stalls
              << " endpoint_total=" << endpoint_stalls
              << " fabric_total=" << fabric_stalls
              << " total=" << all_stalls
              << " backpressure_observed="
              << (all_stalls == 0 ? "no" : "yes") << std::endl;
    std::cout << "TEST_BOTTLENECK global_osd_stalls="
              << global_osd_stalls
              << " target_slot_stalls=" << target_slot_stalls
              << " bandwidth_token_stalls=" << bandwidth_token_stalls
              << " ring_link_stalls=" << ring_link_stalls
              << " dominant=" << dominant_bottleneck << std::endl;
    std::cout << "TEST_FAIRNESS jain_index=" << fairness << std::endl;
    std::cout << "TEST_IDLE ring=" << ring_idle << " demo=" << demo_idle
              << " biu=" << biu_idle << " target=" << target_idle
              << std::endl;

    for (const auto& failure : failures) {
        std::cout << "TEST_FAILURE " << failure << std::endl;
    }
    const bool passed = failures.empty();
    std::cout << "TEST_RESULT case=" << test_case.name
              << " status=" << (passed ? "PASS" : "FAIL")
              << " failures=" << failures.size() << std::endl;
    return passed ? 0 : 1;
}

class ScopedStreamRedirect
{
  public:
    ScopedStreamRedirect(std::ostream& stream, std::streambuf* destination)
        : stream_(stream), original_(stream.rdbuf(destination))
    {
    }

    ~ScopedStreamRedirect()
    {
        stream_.rdbuf(original_);
    }

    ScopedStreamRedirect(const ScopedStreamRedirect&) = delete;
    ScopedStreamRedirect& operator=(const ScopedStreamRedirect&) = delete;

  private:
    std::ostream& stream_;
    std::streambuf* original_;
};

template <typename DemoT>
inline int
run_demo_to_file(const std::string& cfg_file_name,
                 const RingDemoConfig& test_case,
                 const std::string& result_file_name)
{
    std::ofstream result_file(result_file_name,
                              std::ios::out | std::ios::trunc);
    if (!result_file.is_open()) {
        throw std::runtime_error("cannot open result file: " +
                                 result_file_name);
    }

    ScopedStreamRedirect cout_redirect(std::cout, result_file.rdbuf());
    ScopedStreamRedirect cerr_redirect(std::cerr, result_file.rdbuf());
    std::cout << "TM_RING_DEMO_RESULT_FILE " << result_file_name
              << std::endl;
    return run_demo<DemoT>(cfg_file_name, test_case);
}

}  // namespace tm_ring_demo

#endif  // _TM_RING_DEMO_TEST_H_
