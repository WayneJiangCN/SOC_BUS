#include "tm_mesh.h"
#include "tm_mesh_inf.h"
#include "tm_mesh_link.h"
#include "tm_mesh_pld.h"
#include "tm_mesh_router.h"
#include "tm_mesh_target_port.h"

using namespace std;
using namespace tm_engine;

bool
TmMeshFabric::resolve_candidate_route(uint32_t router_id,
                                      TmMeshRouteCandidate& cand)
{
    const uint32_t src_node = tm_mesh_pld_src_node(cand.pld);
    const uint32_t dst_node = tm_mesh_pld_dst_node(cand.pld);

    if (cand.traffic_class == TmMeshRouter::REQ_CLASS) {
        cand.req_type = tm_mesh_pld_req_type(cand.pld);
        cand.out_dir = (router_id == dst_node)
                           ? TmMeshPortDir::LOCAL
                           : topology_->route_direction(router_id, dst_node);
        return true;
    }

    if (cand.traffic_class == TmMeshRouter::WR_DAT_CLASS) {
        cand.req_type = aic_req_type_t::WR_DAT;
        cand.out_dir = (router_id == dst_node)
                           ? TmMeshPortDir::LOCAL
                           : topology_->route_direction(router_id, dst_node);
        return true;
    }

    if (cand.traffic_class == TmMeshRouter::WR_REQ_RSP_CLASS ||
        cand.traffic_class == TmMeshRouter::WR_DAT_RSP_CLASS) {
        cand.out_dir = (router_id == src_node)
                           ? TmMeshPortDir::LOCAL
                           : topology_->route_direction(router_id, src_node);
        return true;
    }

    cand.lane = cand.traffic_class - TmMeshRouter::RD_RSP_BASE_CLASS;
    cand.out_dir = (router_id == src_node)
                       ? TmMeshPortDir::LOCAL
                       : topology_->route_direction(router_id, src_node);
    return true;
}

bool
TmMeshFabric::check_candidate_ready(
    uint32_t router_id, const TmMeshRouteCandidate& cand)
{
    if (cand.out_dir == TmMeshPortDir::LOCAL) {
        if (cand.traffic_class == TmMeshRouter::REQ_CLASS ||
            cand.traffic_class == TmMeshRouter::WR_DAT_CLASS) {
            uint32_t target_id = tm_mesh_pld_target_id(cand.pld);
            auto target_port = target_ports_[target_id];
            if (!target_port->can_accept_request(cand.req_type)) {
                schedule_target_issue_retry(target_id, cand.req_type, 1);
                return false;
            }
        }
        return true;
    }

    uint32_t next_router = topology_->neighbor(router_id, cand.out_dir);
    auto link = get_mesh_link(router_id, cand.out_dir, next_router,
                              tm_mesh_opposite_dir(cand.out_dir));
    return link != nullptr && link->can_send(time());
}

bool
TmMeshFabric::commit_local_router_candidate(
    uint32_t, const TmMeshRouteCandidate& cand)
{
    const uint32_t target_id = tm_mesh_pld_target_id(cand.pld);

    if (cand.traffic_class == TmMeshRouter::REQ_CLASS ||
        cand.traffic_class == TmMeshRouter::WR_DAT_CLASS) {
        auto target_port = target_ports_[target_id];
        if (!target_port->can_accept_request(cand.req_type)) {
            return false;
        }

        target_port->accept_request(cand.req_type, cand.pld);
        return true;
    }

    uint32_t master_port = topology_->find_master_port(cand.pld->mst_id);
    if (master_port >= master_nius_.size() ||
        master_nius_[master_port] == nullptr) {
        return false;
    }
    auto niu = master_nius_[master_port];

    if (cand.traffic_class == TmMeshRouter::WR_REQ_RSP_CLASS) {
        TmMeshGrant grant;
        grant.gid = cand.pld->gid;
        grant.target_id = target_id;
        grant.chan = cand.pld->chan;
        grant.dbid = cand.pld->tnx_id;

        if (!niu->accept_write_request_response(cand.pld, grant)) {
            return false;
        }

        return true;
    }

    if (cand.traffic_class == TmMeshRouter::WR_DAT_RSP_CLASS) {
        if (!niu->accept_write_data_response(cand.pld)) {
            return false;
        }

        flow_ctrl_->release_target_credit(target_id, aic_req_type_t::WR_DAT);
        schedule_target_issue_retry(target_id, aic_req_type_t::WR_REQ, 1);
        schedule_target_issue_retry(target_id, aic_req_type_t::WR_DAT, 1);
        return true;
    }

    if (!niu->accept_read_response(cand.pld, cand.lane)) {
        return false;
    }

    auto key = make_txn_key(cand.pld);
    auto& rd_state = rd_rsp_states_[key];
    if (rd_state.rsp_expected == 1 && cand.pld->latency > 1) {
        rd_state.rsp_expected = cand.pld->latency;
    }
    rd_state.rsp_seen++;

    if (!rd_state.slot_released) {
        flow_ctrl_->release_target_credit(target_id, aic_req_type_t::RD_REQ);
        schedule_target_issue_retry(target_id, aic_req_type_t::RD_REQ, 1);
        schedule_target_issue_retry(target_id, aic_req_type_t::WR_REQ, 1);
        rd_state.slot_released = true;
    }

    if (rd_state.rsp_seen >= rd_state.rsp_expected) {
        rd_rsp_states_.erase(key);
    }
    return true;
}

bool
TmMeshFabric::commit_link_router_candidate(
    uint32_t router_id, const TmMeshRouteCandidate& cand)
{
    uint32_t next_router = topology_->neighbor(router_id, cand.out_dir);
    auto dst_dir = tm_mesh_opposite_dir(cand.out_dir);
    auto link = get_mesh_link(router_id, cand.out_dir, next_router, dst_dir);
    if (link == nullptr || !link->can_send(time())) {
        return false;
    }

    link->enqueue(cand.pld, cand.traffic_class, time());
    return true;
}

bool
TmMeshFabric::commit_router_candidate(
    uint32_t router_id, const TmMeshRouteCandidate& cand)
{
    if (!cand.valid || cand.pld == nullptr) {
        return false;
    }

    if (cand.out_dir == TmMeshPortDir::LOCAL) {
        return commit_local_router_candidate(router_id, cand);
    }
    return commit_link_router_candidate(router_id, cand);
}

void
TmMeshFabric::send_target_reqs()
{
    for (uint32_t target = 0; target < cfg_->num_targets; ++target) {
        send_target_req(target, aic_req_type_t::RD_REQ);
        send_target_req(target, aic_req_type_t::WR_REQ);
        send_target_req(target, aic_req_type_t::WR_DAT);
    }
}

void
TmMeshFabric::schedule_target_issue_retry(uint32_t target_id,
                                          aic_req_type_t req_type,
                                          tm_time_t delay)
{
    if (target_id >= target_ports_.size() || target_ports_[target_id] == nullptr) {
        return;
    }
    target_ports_[target_id]->notify_issue_retry(req_type, delay);
}

void
TmMeshFabric::send_target_req(uint32_t target_id, aic_req_type_t req_type)
{
    auto target_port = target_ports_[target_id];
    if (!target_port->has_request(req_type)) {
        return;
    }

    auto pld = target_port->front_request(req_type);
    if (!flow_ctrl_->can_send_to_target(target_id, req_type, pld)) {
        schedule_target_issue_retry(target_id, req_type, 1);
        return;
    }

    tm_time_t* next_issue = nullptr;
    if (req_type == aic_req_type_t::RD_REQ) {
        next_issue = &next_rd_issue_time_[target_id];
    } else if (req_type == aic_req_type_t::WR_REQ) {
        next_issue = &next_wr_req_issue_time_[target_id];
    } else {
        next_issue = &next_wr_dat_issue_time_[target_id];
    }

    if (time() < *next_issue) {
        schedule_target_issue_retry(target_id, req_type, *next_issue - time());
        return;
    }

    if (target_port->send_request(req_type, pld)) {
        target_port->pop_request(req_type);
        flow_ctrl_->consume_target_credit(target_id, req_type, pld);

        if (req_type == aic_req_type_t::RD_REQ) {
            rd_rsp_states_[make_txn_key(pld)] = TmMeshRdRspState();
        }

        if (req_type == aic_req_type_t::WR_DAT) {
            uint32_t master_port = topology_->find_master_port(pld->mst_id);
            if (master_port < master_nius_.size() &&
                master_nius_[master_port] != nullptr) {
                master_nius_[master_port]->pop_pending_grant();
            }
        }

        *next_issue =
            time() + flow_ctrl_->calc_issue_busy_cycles(target_id, pld);
        if (target_port->has_request(req_type)) {
            schedule_target_issue_retry(target_id, req_type,
                                        *next_issue - time());
        }
    } else {
        schedule_target_issue_retry(target_id, req_type, 1);
    }
}
