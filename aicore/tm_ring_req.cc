#include "tm_ring.h"
#include "tm_ring_inf.h"
#include "tm_pld.h"

using namespace std;
using namespace tm_engine;

void
TmRingFabric::complete_master_response(p_tm_pld_t pld)
{
    const uint32_t target_id = tm_pld_target_id(pld);
    auto cmd = static_cast<PldCmd>(pld->ring_traffic_class);

    if (cmd == PldCmd::WR_RSP) {
        return;
    }

    uint32_t master_port = topology_->find_master_port(pld->mst_id);
    if (master_port >= master_nius_.size() ||
        master_nius_[master_port] == nullptr) {
        return;
    }
    auto niu = master_nius_[master_port];

    if (cmd == PldCmd::RSP) {
        niu->release_write_osd();
        flow_ctrl_->release_target_credit(target_id,
                                          tm_ring_cmd_to_req(PldCmd::WR_DAT));
        return;
    }

    auto key = make_txn_key(pld);
    auto& rd_state = rd_rsp_states_[key];
    if (rd_state.rsp_expected == 1 && tm_pld_rsp_count(pld) > 1) {
        rd_state.rsp_expected = tm_pld_rsp_count(pld);
    }
    rd_state.rsp_seen++;

    if (!rd_state.slot_released) {
        flow_ctrl_->release_target_credit(target_id,
                                          tm_ring_cmd_to_req(PldCmd::RD));
        rd_state.slot_released = true;
    }

    if (rd_state.rsp_seen >= rd_state.rsp_expected) {
        niu->release_read_osd();
        rd_rsp_states_.erase(key);
    }
}
