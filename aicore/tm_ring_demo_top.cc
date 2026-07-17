#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "cfg.h"
#include "pem_trdemo.h"
#include "pem_biu.h"
#include "tm_engine.h"
#include "tm_mem.h"
#include "tm_ring.h"
#include "types.h"

using namespace std;
using namespace tm_engine;

namespace {

constexpr uint64_t kDemoSrcAddr = 0x30000000ull;
constexpr uint64_t kDemoDstAddr = 0x40000000ull;
constexpr uint64_t kMasterAddrStride = 0x01000000ull;
constexpr uint32_t kDemoCycles = 20000;

void before_test() {}
void after_test() {}

struct RingDemoCase
{
    std::string name = "single_rw";
    uint32_t num_masters = 1;
    uint32_t num_targets = 1;
    uint32_t uops_per_master = TOTAL_UOP_COUNT;
    uint32_t cycles = kDemoCycles;
    bool target_interleave = false;
    bool small_buffers = false;
};

RingDemoCase
make_demo_case(const std::string& name)
{
    RingDemoCase tc;
    tc.name = name;

    if (name == "multi_master") {
        tc.num_masters = 4;
        tc.uops_per_master = 256;
        tc.cycles = 40000;
    } else if (name == "multi_target_linear") {
        tc.num_masters = 4;
        tc.num_targets = 4;
        tc.uops_per_master = 256;
        tc.cycles = 50000;
        tc.target_interleave = true;
    } else if (name == "backpressure") {
        tc.num_masters = 4;
        tc.num_targets = 2;
        tc.uops_per_master = 256;
        tc.cycles = 60000;
        tc.target_interleave = true;
        tc.small_buffers = true;
    }

    return tc;
}

p_tm_ring_target_cfg_t
make_ddr_target_cfg(const p_tm_mem_cfg_t& ddr_cfg,
                    const RingDemoCase& tc,
                    uint32_t target_id)
{
    auto target = std::make_shared<tm_ring_target_cfg_t>();

    target->name = "ddr" + std::to_string(target_id);
    target->addr_begin = 0;
    target->size = 0x80000000ull;
    target->is_default = !tc.target_interleave && target_id == 0;
    if (tc.target_interleave) {
        target->interleave_type = tm_bus_interleave_type_t::LINEAR;
        target->interleave_size = BAND_WIDTH;
        target->interleave_num = tc.num_targets;
        target->interleave_idx = target_id;
    }

    target->frontend_latency = 1;
    target->forward_latency = 0;
    target->response_latency = 1;
    target->header_latency = 1;
    target->width = 32;

    target->rd_req_fifo_depth = tc.small_buffers ? 2 : 16;
    target->wr_req_fifo_depth = tc.small_buffers ? 2 : 16;
    target->wr_dat_fifo_depth = tc.small_buffers ? 2 : 16;

    target->rd_slot_credit_max = ddr_cfg->ddr->max_rd_crdt;
    target->wr_slot_credit_max = ddr_cfg->ddr->max_wr_crdt;
    target->acc_slot_credit_max = ddr_cfg->ddr->max_acc_crdt;

    target->acc_bw_token_max = ddr_cfg->ddr->max_acc_crdt;
    target->rd_bw_token_max = ddr_cfg->ddr->max_rd_crdt;
    target->wr_bw_token_max = ddr_cfg->ddr->max_wr_crdt;
    target->token_update_period = ddr_cfg->ddr->crdt_update_period;
    target->acc_bw_token_update = ddr_cfg->ddr->acc_crdt_update;
    target->rd_bw_token_update = ddr_cfg->ddr->rd_crdt_update;
    target->wr_bw_token_update = ddr_cfg->ddr->wr_crdt_update;

    return target;
}

p_tm_ring_cfg_t
make_demo_ring_cfg(const p_tm_mem_cfg_t& ddr_cfg, const RingDemoCase& tc)
{
    auto cfg = tm_make_ring_cfg();

    cfg->name = "demo_ring_" + tc.name;
    cfg->num_masters = tc.num_masters;
    cfg->num_targets = tc.num_targets;
    cfg->rd_rsp_port_num = 2;

    cfg->master_inf_depth = 2;
    cfg->target_inf_depth = 2;
    cfg->ring_router_input_depth = 2;
    cfg->ring_link_inf_depth = 2;

    cfg->master_rd_cmd_fifo_depth = tc.small_buffers ? 2 : 16;
    cfg->master_wr_cmd_fifo_depth = tc.small_buffers ? 2 : 16;
    cfg->master_wr_dat_fifo_depth = tc.small_buffers ? 2 : 16;
    cfg->master_wr_rsp_fifo_depth = tc.small_buffers ? 2 : 16;
    cfg->master_rd_osd = 16;
    cfg->master_wr_osd = 16;
    cfg->global_osd = 64;

    cfg->ring_req_fifo_depth = tc.small_buffers ? 2 : 8;
    cfg->ring_rsp_fifo_depth = tc.small_buffers ? 2 : 8;
    cfg->ring_link_latency = 1;
    cfg->ring_link_width_bytes = 16;
    cfg->ring_req_header_bytes = 16;
    cfg->ring_rsp_header_bytes = 16;
    cfg->ring_req_link_max_inflight = 8;
    cfg->ring_rsp_link_max_inflight = 8;

    for (uint32_t target = 0; target < tc.num_targets; ++target) {
        cfg->targets.push_back(make_ddr_target_cfg(ddr_cfg, tc, target));
    }
    return cfg;
}

void
preload_demo_data(const std::vector<p_tm_mem_t>& targets,
                  uint64_t addr,
                  uint32_t bytes)
{
    std::vector<uint8_t> data(bytes, 1);
    for (auto& target : targets) {
        target->pv_write(addr, bytes, data.data(), false);
    }
}

p_isa_t
make_demo_instr(uint64_t src_addr, uint64_t dst_addr, uint32_t uops)
{
    auto instr = make_shared<isa_t>();
    instr->get_binfo()->name_ = isa_name_t::ZADD;
    instr->start_addr_ = src_addr;
    instr->end_addr_ = dst_addr;
    instr->number_size_ = uops;
    return instr;
}

}  // namespace

int
main(int argc, char** argv)
{
    const std::string cfg_file_name =
        argc > 1 ? argv[1] : std::string("pem_config_cloud.toml");
    const std::string case_name =
        argc > 2 ? argv[2] : std::string("single_rw");
    auto test_case = make_demo_case(case_name);

    before_test();
    tm_init();

    auto clk = tm_make_clk();

    auto demo_cfg = make_shared<cfg::Cfg>();
    demo_cfg->read_cfg_file(cfg_file_name);

    auto ddr_cfg = tm_make_mem_cfg(std::string("ddr"), demo_cfg);
    std::vector<p_tm_mem_t> targets;
    for (uint32_t target = 0; target < test_case.num_targets; ++target) {
        auto mem_cfg = tm_make_mem_cfg("ddr" + std::to_string(target),
                                       demo_cfg);
        targets.push_back(tm_make_mem(clk, mem_cfg));
    }

    for (uint32_t master = 0; master < test_case.num_masters; ++master) {
        uint64_t src_addr = kDemoSrcAddr + master * kMasterAddrStride;
        uint32_t bytes = test_case.uops_per_master * BAND_WIDTH;
        preload_demo_data(targets, src_addr, bytes);
    }

    auto ring_cfg = make_demo_ring_cfg(ddr_cfg, test_case);
    auto ring = tm_make_ring(clk, ring_cfg);
    ring->build();

    std::vector<p_pem_biu_t> bius;
    std::vector<std::shared_ptr<PemTrDemo>> demos;
    for (uint32_t master = 0; master < test_case.num_masters; ++master) {
        auto biu = make_shared<pem_biu_t>("biu" + std::to_string(master), clk,
                                          demo_cfg->get_cfg_tab("BIU"));
        biu->core_id_ = master;
        biu->build();
        ring->attach_master(master, biu);
        bius.push_back(biu);

        uint64_t src_addr = kDemoSrcAddr + master * kMasterAddrStride;
        uint64_t dst_addr = kDemoDstAddr + master * kMasterAddrStride;
        auto demo = make_shared<PemTrDemo>("pem_trdemo" + std::to_string(master),
                                           clk);
        demo->configure_traffic(src_addr, dst_addr, test_case.uops_per_master);
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
        uint64_t src_addr = kDemoSrcAddr + master * kMasterAddrStride;
        uint64_t dst_addr = kDemoDstAddr + master * kMasterAddrStride;
        demos[master]->instr_que_->push_back(
            make_demo_instr(src_addr, dst_addr, test_case.uops_per_master));
    }

    tm_start(test_case.cycles);
    stats::stat->dump();

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

    std::cout << "case: " << test_case.name << std::endl;
    std::cout << "masters: " << test_case.num_masters
              << ", targets: " << test_case.num_targets << std::endl;
    std::cout << "ring idle: " << ring->idle() << std::endl;
    std::cout << "demo idle: " << demo_idle << std::endl;
    std::cout << "biu idle: " << biu_idle << std::endl;
    std::cout << "target idle: " << target_idle << std::endl;

    after_test();
    return 0;
}
