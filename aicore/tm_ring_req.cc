#include "tm_ring.h"
#include "tm_ring_inf.h"
#include "tm_ring_link.h"
#include "tm_ring_router.h"
#include "tm_ring_target_port.h"
#include "tm_pld.h"

#include <iostream>

using namespace std;
using namespace tm_engine;

bool
TmRingFabric::can_accept_local(p_tm_pld_t pld)
{
    auto traffic_class = pld->ring_traffic_class;
    auto cmd = static_cast<PldCmd>(traffic_class);
    auto req_type = cmd == PldCmd::WR_DAT
                        ? aic_req_type_t::WR_DAT
                        : static_cast<aic_req_type_t>(tm_pld_req_type(pld));

    if (cmd == PldCmd::RD || cmd == PldCmd::WR || cmd == PldCmd::WR_DAT) {
        uint32_t target_id = tm_pld_target_id(pld);
        return target_ports_[target_id]->can_accept_request(req_type);
    }
    return true;
}

bool
TmRingFabric::accept_local(p_tm_pld_t pld)
{
    const uint32_t target_id = tm_pld_target_id(pld);
    auto traffic_class = pld->ring_traffic_class;
    auto cmd = static_cast<PldCmd>(traffic_class);
    auto req_type = cmd == PldCmd::WR_DAT
                        ? aic_req_type_t::WR_DAT
                        : static_cast<aic_req_type_t>(tm_pld_req_type(pld));

    if (cmd == PldCmd::RD || cmd == PldCmd::WR || cmd == PldCmd::WR_DAT) {
        auto target_port = target_ports_[target_id];
        if (!target_port->can_accept_request(req_type)) {
            return false;
        }

        target_port->accept_request(req_type, pld);
        if (req_type == aic_req_type_t::WR_DAT) {
            uint32_t master_port = topology_->find_master_port(pld->mst_id);
            if (master_port < master_nius_.size() &&
                master_nius_[master_port] != nullptr) {
                master_nius_[master_port]->pop_pending_grant();
            }
        }
        return true;
    }

    uint32_t master_port = topology_->find_master_port(pld->mst_id);
    if (master_port >= master_nius_.size() ||
        master_nius_[master_port] == nullptr) {
        return false;
    }
    auto niu = master_nius_[master_port];

    if (cmd == PldCmd::WR_RSP) {
        if (!niu->accept_write_request_response(pld)) {
            return false;
        }

        return true;
    }

    if (cmd == PldCmd::RSP) {
        if (!niu->accept_write_data_response(pld)) {
            return false;
        }

        niu->release_write_osd();
        release_global_osd(aic_req_type_t::WR_REQ);
        flow_ctrl_->release_target_credit(target_id, aic_req_type_t::WR_DAT);
        return true;
    }

    if (!niu->accept_read_response(pld, pld->ring_rsp_lane)) {
        return false;
    }

    auto key = make_txn_key(pld);
    auto& rd_state = rd_rsp_states_[key];
    if (rd_state.rsp_expected == 1 && tm_pld_rsp_count(pld) > 1) {
        rd_state.rsp_expected = tm_pld_rsp_count(pld);
    }
    rd_state.rsp_seen++;

    if (!rd_state.slot_released) {
        flow_ctrl_->release_target_credit(target_id, aic_req_type_t::RD_REQ);
        rd_state.slot_released = true;
    }

    if (rd_state.rsp_seen >= rd_state.rsp_expected) {
        niu->release_read_osd();
        release_global_osd(aic_req_type_t::RD_REQ);
        rd_rsp_states_.erase(key);
    }
    return true;
}

bool
TmRingFabric::reserve_global_osd(aic_req_type_t req_type)
{
    if (req_type != aic_req_type_t::RD_REQ &&
        req_type != aic_req_type_t::WR_REQ) {
        return true;
    }
    if (global_outstanding_ >= cfg_->global_osd) {
        return false;
    }
    global_outstanding_++;
    return true;
}

void
TmRingFabric::release_global_osd(aic_req_type_t req_type)
{
    if (req_type != aic_req_type_t::RD_REQ &&
        req_type != aic_req_type_t::WR_REQ) {
        return;
    }
    if (global_outstanding_ == 0) {
        std::cerr << this->name()
                  << ": global outstanding underflow on release" << std::endl;
        return;
    }
    global_outstanding_--;
}
