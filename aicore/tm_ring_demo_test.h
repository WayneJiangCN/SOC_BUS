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
#include "tm_ring_demo_config.h"
#include "types.h"

namespace tm_ring_demo {

using namespace tm_engine;

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

    target->width = tc.target_width_bytes;
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
    cfg->rd_rsp_port_num = 2;

    cfg->master_rd_cmd_fifo_depth = tc.master_fifo_depth;
    cfg->master_wr_cmd_fifo_depth = tc.master_fifo_depth;
    cfg->master_wr_dat_fifo_depth = tc.master_fifo_depth;
    cfg->master_wr_rsp_fifo_depth = tc.master_fifo_depth;
    cfg->master_rd_osd = tc.master_rd_osd;
    cfg->master_wr_osd = tc.master_wr_osd;
    cfg->global_osd = tc.global_osd;

    cfg->ring_link_latency = tc.ring_link_latency;
    cfg->ring_link_width_bytes = tc.ring_link_width_bytes;
    cfg->ring_router_input_depth = tc.ring_router_input_depth;

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
run_demo(const std::string& ddr_config_file,
         const RingDemoConfig& test_case)
{
    tm_init();
    auto clk = tm_make_clk();

    std::ifstream ddr_config_probe(ddr_config_file);
    if (!ddr_config_probe.is_open()) {
        throw std::runtime_error(
            "cannot open DDR/PEM config file: " + ddr_config_file);
    }
    ddr_config_probe.close();

    auto demo_cfg = std::make_shared<cfg::Cfg>();
    demo_cfg->read_cfg_file(ddr_config_file);

    const char* required_tables[] = {"ARCH", "DDR", "L2", "BIU"};
    for (const char* table : required_tables) {
        if (demo_cfg->get_cfg_tab(table) == nullptr) {
            throw std::runtime_error(
                "DDR/PEM config is missing required [" +
                std::string(table) + "] configuration: " +
                ddr_config_file);
        }
    }
    auto biu_cfg = demo_cfg->get_cfg_tab("BIU");
    auto ddr_cfg = tm_make_mem_cfg(std::string("ddr"), demo_cfg);
    if (ddr_cfg == nullptr || ddr_cfg->ddr == nullptr) {
        throw std::runtime_error(
            "failed to construct DDR configuration from: " +
            ddr_config_file);
    }

    std::vector<p_tm_mem_t> targets;
    for (uint32_t target = 0; target < test_case.num_targets; ++target) {
        auto mem_cfg = tm_make_mem_cfg(
            "ddr" + std::to_string(target), demo_cfg);
        targets.push_back(tm_make_mem(clk, mem_cfg));
    }

    std::vector<std::string> failures;

    auto ring_cfg = make_demo_ring_cfg(ddr_cfg, test_case);
    auto ring = tm_make_ring(clk, ring_cfg);
    ring->build();

    std::vector<p_pem_biu_t> bius;
    std::vector<std::shared_ptr<DemoT>> demos;
    for (uint32_t master = 0; master < test_case.num_masters; ++master) {
        auto biu = std::make_shared<pem_biu_t>(
            "biu" + std::to_string(master), clk, biu_cfg);
        biu->core_id_ = master;
        biu->build();
        biu->reset();
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

    // Ring 在构造阶段已经 reset；attach 后不要再次 reset 已连接的 TmInf。
    for (auto& demo : demos) {
        demo->reset();
    }

    // PV memory initialization must happen after every model/interface reset.
    // Otherwise a reset in the build/attach sequence can silently restore the
    // memory reset value and all read data becomes zero.
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
              << " ddr_config=" << ddr_config_file
              << " masters=" << test_case.num_masters
              << " targets=" << test_case.num_targets
              << " uops_per_master=" << test_case.uops_per_master
              << " cycle_limit=" << test_case.cycles
              << " clock_ghz=" << test_case.clock_ghz << std::endl;
    std::cout << "TEST_BUS_CONFIG interleave="
              << interleave_name(test_case)
              << " interleave_size=" << test_case.interleave_size
              << " link_latency=" << test_case.ring_link_latency
              << " link_capacity=" << (test_case.ring_link_latency + 1)
              << " link_width=" << test_case.ring_link_width_bytes
              << " router_input_depth=" << test_case.ring_router_input_depth
              << " target_width=" << test_case.target_width_bytes
              << " target_latency="
              << (ring_cfg->targets[0]->frontend_latency +
                  ring_cfg->targets[0]->forward_latency +
                  ring_cfg->targets[0]->response_latency +
                  ring_cfg->targets[0]->header_latency)
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
              << " hotspot_threshold="
              << ring_cfg->targets[0]->hotspot_threshold
              << " hotspot_penalty=" << ring_cfg->targets[0]->hotspot_penalty
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
        uint64_t last_response_cycle = 0;
        bool has_last_response_cycle = false;
        if (stat.has_last_read_response_cycle) {
            last_response_cycle =
                std::max(last_response_cycle, stat.last_read_response_cycle);
            has_last_response_cycle = true;
        }
        if (stat.has_last_write_response_cycle) {
            last_response_cycle =
                std::max(last_response_cycle, stat.last_write_response_cycle);
            has_last_response_cycle = true;
        }
        if (stat.has_first_read_cycle && has_last_response_cycle &&
            last_response_cycle >= stat.first_read_cycle) {
            active_cycles = last_response_cycle - stat.first_read_cycle + 1;
            global_last_cycle = std::max(global_last_cycle,
                                         last_response_cycle);
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
    const TmRingLinkStallBreakdown ring_link_breakdown =
        ring->ring_link_stall_breakdown();
    const uint64_t ring_link_stalls = ring_link_breakdown.total();
    const uint64_t fabric_stalls = global_osd_stalls + target_slot_stalls +
                                   bandwidth_token_stalls + ring_link_stalls;
    const uint64_t all_stalls = endpoint_stalls + fabric_stalls;
    if (test_case.name == "backpressure" && all_stalls == 0) {
        failures.push_back(
            "backpressure case completed without exercising a stall path");
    }
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
        dominant_bottleneck = "ring_link_serialization_busy";
        uint64_t dominant_link_stalls =
            ring_link_breakdown.serialization_busy;
        if (ring_link_breakdown.inflight_limit > dominant_link_stalls) {
            dominant_link_stalls = ring_link_breakdown.inflight_limit;
            dominant_bottleneck = "ring_link_inflight_limit";
        }
        if (ring_link_breakdown.link_fifo_full > dominant_link_stalls) {
            dominant_link_stalls = ring_link_breakdown.link_fifo_full;
            dominant_bottleneck = "ring_link_fifo_full";
        }
        if (ring_link_breakdown.bubble_reserved > dominant_link_stalls) {
            dominant_link_stalls = ring_link_breakdown.bubble_reserved;
            dominant_bottleneck = "ring_link_bubble_reserved";
        }
        if (ring_link_breakdown.downstream_fifo_full >
            dominant_link_stalls) {
            dominant_bottleneck = "ring_link_downstream_fifo_full";
        }
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
              << " ring_link_serialization_busy_stalls="
              << ring_link_breakdown.serialization_busy
              << " ring_link_inflight_limit_stalls="
              << ring_link_breakdown.inflight_limit
              << " ring_link_fifo_full_stalls="
              << ring_link_breakdown.link_fifo_full
              << " ring_link_bubble_stalls="
              << ring_link_breakdown.bubble_reserved
              << " ring_link_downstream_fifo_full_stalls="
              << ring_link_breakdown.downstream_fifo_full
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
run_demo_to_file(const std::string& ddr_config_file,
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
    return run_demo<DemoT>(ddr_config_file, test_case);
}

}  // namespace tm_ring_demo

#endif  // _TM_RING_DEMO_TEST_H_
