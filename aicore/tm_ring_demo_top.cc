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
constexpr uint32_t kDemoBytes = 204800;
constexpr uint32_t kDemoCycles = 20000;

void before_test() {}
void after_test() {}

p_tm_ring_target_cfg_t
make_ddr_target_cfg(const p_tm_mem_cfg_t& ddr_cfg)
{
    auto target = std::make_shared<tm_ring_target_cfg_t>();

    target->name = "ddr";
    target->addr_begin = 0;
    target->size = 0x80000000ull;
    target->is_default = true;

    target->frontend_latency = 1;
    target->forward_latency = 0;
    target->response_latency = 1;
    target->header_latency = 1;
    target->width = 32;

    target->rd_req_fifo_depth = 16;
    target->wr_req_fifo_depth = 16;
    target->wr_dat_fifo_depth = 16;

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
make_demo_ring_cfg(const p_tm_mem_cfg_t& ddr_cfg)
{
    auto cfg = tm_make_ring_cfg();

    cfg->name = "demo_ring";
    cfg->num_masters = 1;
    cfg->num_targets = 1;
    cfg->rd_rsp_port_num = 2;

    cfg->master_inf_depth = 2;
    cfg->target_inf_depth = 2;
    cfg->ring_router_input_depth = 2;
    cfg->ring_link_inf_depth = 2;

    cfg->master_rd_cmd_fifo_depth = 16;
    cfg->master_wr_cmd_fifo_depth = 16;
    cfg->master_wr_dat_fifo_depth = 16;
    cfg->master_wr_rsp_fifo_depth = 16;
    cfg->master_rd_osd = 16;
    cfg->master_wr_osd = 16;
    cfg->global_osd = 64;

    cfg->ring_req_fifo_depth = 8;
    cfg->ring_rsp_fifo_depth = 8;
    cfg->ring_link_latency = 1;
    cfg->ring_link_width_bytes = 16;
    cfg->ring_req_header_bytes = 16;
    cfg->ring_rsp_header_bytes = 16;
    cfg->ring_req_link_max_inflight = 8;
    cfg->ring_rsp_link_max_inflight = 8;

    cfg->targets.push_back(make_ddr_target_cfg(ddr_cfg));
    return cfg;
}

void
preload_demo_data(const p_tm_mem_t& ddr)
{
    std::vector<uint8_t> data(kDemoBytes, 1);
    ddr->pv_write(kDemoSrcAddr, kDemoBytes, data.data(), false);
}

p_isa_t
make_demo_instr()
{
    auto instr = make_shared<isa_t>();
    instr->get_binfo()->name_ = isa_name_t::ZADD;
    instr->start_addr_ = kDemoSrcAddr;
    instr->end_addr_ = kDemoDstAddr;
    instr->number_size_ = 2048;
    return instr;
}

}  // namespace

int
main(int argc, char** argv)
{
    const std::string cfg_file_name =
        argc > 1 ? argv[1] : std::string("pem_config_cloud.toml");

    before_test();
    tm_init();

    auto clk = tm_make_clk();

    auto demo_cfg = make_shared<cfg::Cfg>();
    demo_cfg->read_cfg_file(cfg_file_name);

    auto ddr_cfg = tm_make_mem_cfg(std::string("ddr"), demo_cfg);
    auto ddr = tm_make_mem(clk, ddr_cfg);
    preload_demo_data(ddr);

    auto ring_cfg = make_demo_ring_cfg(ddr_cfg);
    auto ring = tm_make_ring(clk, ring_cfg);
    ring->build();

    auto biu = make_shared<pem_biu_t>(std::string("biu"), clk,
                                      demo_cfg->get_cfg_tab("BIU"));
    biu->build();

    ring->attach_master(0, biu);
    ring->attach_target(0, ddr);

    auto demo = make_shared<PemTrDemo>(std::string("pem_trdemo"), clk);
    demo->attach(biu);
    demo->build();

    ring->reset();
    biu->reset();
    demo->reset();

    demo->instr_que_->push_back(make_demo_instr());

    tm_start(kDemoCycles);
    stats::stat->dump();

    std::cout << "ring idle: " << ring->idle() << std::endl;

    after_test();
    return 0;
}
