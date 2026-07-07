#include "tm_mesh.h"

#include <unordered_set>

#include "tm_mesh_inf.h"

using namespace std;
using namespace tm_engine;

namespace
{

uintptr_t
mesh_packet_tag(p_tm_pld_t pld)
{
    return reinterpret_cast<uintptr_t>(pld.get());
}

}  // namespace

void
TmMeshFabric::recv_master_reqs()
{
    /*
     * master 本地的请求 FIFO 已经移到 Tm_mesh_inf 中。
     * fabric 这里只负责让每个 NIU 从上游接口吸包，并在必要时建立事务上下文。
     */
    for (uint32_t master = 0; master < cfg_->num_masters; ++master) {
        if (master >= v_master_niu_.size() || v_master_niu_[master] == nullptr) {
            continue;
        }
        v_master_niu_[master]->ingest_upstream_reqs(master, topology_, txn_ctx_,
                                                    time());
    }
}

void
TmMeshFabric::inject_mesh_reqs()
{
    /*
     * RD_REQ 和 WR_REQ 共用 request subnet。
     * WR_DAT 单独走 data subnet。
     */
    for (uint32_t master = 0; master < cfg_->num_masters; ++master) {
        inject_mesh_req(master, aic_req_type_t::RD_REQ);
        inject_mesh_req(master, aic_req_type_t::WR_REQ);
        inject_mesh_req(master, aic_req_type_t::WR_DAT);
    }
}

void
TmMeshFabric::inject_mesh_req(uint32_t master_port, aic_req_type_t req_type)
{
    if (master_port >= v_master_niu_.size() || v_master_niu_[master_port] == nullptr) {
        return;
    }

    auto niu = v_master_niu_[master_port];
    auto router_id = topology_.master_node(master_port);
    auto mesh_fifo = get_mesh_req_fifo(router_id, req_type);

    while (niu->has_req(req_type) && !mesh_fifo->full()) {
        auto pld = niu->front_req(req_type);
        if (pld == nullptr) {
            return;
        }

        auto key = make_txn_key(pld);
        auto ctx_it = txn_ctx_.find(key);
        if (ctx_it == txn_ctx_.end()) {
            if (req_type == aic_req_type_t::WR_DAT) {
                return;
            }

            TmMeshTxnCtx ctx;
            ctx.master_port = master_port;
            ctx.target_id = topology_.decode_target(pld->addr);
            ctx.src_node = topology_.master_node(master_port);
            ctx.dst_node = topology_.target_node(ctx.target_id);
            ctx.req_type = req_type;
            ctx.state = tm_mesh_txn_state_t::IN_INGRESS_FIFO;
            ctx.size = pld->size;
            ctx.issue_time = time();
            ctx_it = txn_ctx_.emplace(key, ctx).first;
        }

        if (req_type == aic_req_type_t::WR_DAT) {
            /*
             * WR_DAT 必须等对应 grant 到位后，才能从 NIU 注入 data subnet。
             */
            if (!niu->has_grant()) {
                return;
            }
            const auto& grant = niu->front_grant();
            if (!flow_ctrl_.wr_grant_match(grant.target_id, grant.gid,
                                           ctx_it->second.target_id, pld,
                                           cfg_->strict_wr_grant_order)) {
                return;
            }
        }

        niu->pop_req(req_type);
        mesh_fifo->push_back(pld);
        ctx_it->second.state = tm_mesh_txn_state_t::IN_REQ_MESH;
    }
}

void
TmMeshFabric::advance_mesh_reqs()
{
    advance_mesh_req_subnet();
    advance_mesh_wr_dat_subnet();
}

void
TmMeshFabric::advance_mesh_req_subnet()
{
    std::unordered_set<uintptr_t> moved_tags;

    for (uint32_t router = 0; router < mesh_router_count_; ++router) {
        auto fifo = mesh_req_fifo_[router];
        if (fifo->empty()) {
            continue;
        }

        auto pld = fifo->front();
        uintptr_t tag = mesh_packet_tag(pld);
        if (moved_tags.find(tag) != moved_tags.end()) {
            continue;
        }

        auto key = make_txn_key(pld);
        auto ctx_it = txn_ctx_.find(key);
        if (ctx_it == txn_ctx_.end()) {
            continue;
        }

        auto actual_req_type = ctx_it->second.req_type;
        if (router == ctx_it->second.dst_node) {
            auto target_fifo =
                get_target_fifo(ctx_it->second.target_id, actual_req_type);
            if (target_fifo->full()) {
                continue;
            }

            fifo->pop_front();
            target_fifo->push_back(pld);
            ctx_it->second.state = tm_mesh_txn_state_t::IN_TARGET_FIFO;
            moved_tags.insert(tag);
            continue;
        }

        if (time() < next_mesh_req_hop_time_[router]) {
            continue;
        }

        uint32_t next_router =
            topology_.compute_next_node(router, ctx_it->second.dst_node);
        if (next_router == router) {
            continue;
        }

        auto next_fifo = mesh_req_fifo_[next_router];
        if (next_fifo->full()) {
            continue;
        }

        fifo->pop_front();
        next_fifo->push_back(pld);
        next_mesh_req_hop_time_[router] = time() + mesh_link_latency_;
        ctx_it->second.state = tm_mesh_txn_state_t::IN_REQ_MESH;
        moved_tags.insert(tag);
    }
}

void
TmMeshFabric::advance_mesh_wr_dat_subnet()
{
    std::unordered_set<uintptr_t> moved_tags;

    for (uint32_t router = 0; router < mesh_router_count_; ++router) {
        auto fifo = mesh_wr_dat_fifo_[router];
        if (fifo->empty()) {
            continue;
        }

        auto pld = fifo->front();
        uintptr_t tag = mesh_packet_tag(pld);
        if (moved_tags.find(tag) != moved_tags.end()) {
            continue;
        }

        auto key = make_txn_key(pld);
        auto ctx_it = txn_ctx_.find(key);
        if (ctx_it == txn_ctx_.end()) {
            continue;
        }

        if (router == ctx_it->second.dst_node) {
            auto target_fifo =
                get_target_fifo(ctx_it->second.target_id, aic_req_type_t::WR_DAT);
            if (target_fifo->full()) {
                continue;
            }

            fifo->pop_front();
            target_fifo->push_back(pld);
            ctx_it->second.state = tm_mesh_txn_state_t::IN_TARGET_FIFO;
            moved_tags.insert(tag);
            continue;
        }

        if (time() < next_mesh_wr_dat_hop_time_[router]) {
            continue;
        }

        uint32_t next_router =
            topology_.compute_next_node(router, ctx_it->second.dst_node);
        if (next_router == router) {
            continue;
        }

        auto next_fifo = mesh_wr_dat_fifo_[next_router];
        if (next_fifo->full()) {
            continue;
        }

        fifo->pop_front();
        next_fifo->push_back(pld);
        next_mesh_wr_dat_hop_time_[router] = time() + mesh_link_latency_;
        ctx_it->second.state = tm_mesh_txn_state_t::IN_REQ_MESH;
        moved_tags.insert(tag);
    }
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
TmMeshFabric::send_target_req(uint32_t target_id, aic_req_type_t req_type)
{
    auto target_fifo = get_target_fifo(target_id, req_type);
    if (target_fifo->empty()) {
        return;
    }

    auto pld = target_fifo->front();
    if (!flow_ctrl_.can_send_to_target(target_id, req_type, pld)) {
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
        return;
    }

    if (v_target_inf_[target_id]->send(static_cast<uint32_t>(req_type), pld)) {
        target_fifo->pop_front();
        flow_ctrl_.consume_target_credit(target_id, req_type, pld);

        auto key = make_txn_key(pld);
        auto ctx_it = txn_ctx_.find(key);
        if (ctx_it != txn_ctx_.end()) {
            if (req_type == aic_req_type_t::RD_REQ) {
                ctx_it->second.state = tm_mesh_txn_state_t::WAIT_RD_RSP;
            } else if (req_type == aic_req_type_t::WR_REQ) {
                ctx_it->second.state = tm_mesh_txn_state_t::WAIT_WR_REQ_RSP;
            } else {
                ctx_it->second.state = tm_mesh_txn_state_t::WAIT_WR_DAT_RSP;
            }
        }

        if (req_type == aic_req_type_t::WR_DAT) {
            uint32_t master_port = topology_.find_master_port(pld->mst_id);
            if (master_port < v_master_niu_.size() &&
                v_master_niu_[master_port] != nullptr) {
                v_master_niu_[master_port]->pop_grant();
            }
        }

        *next_issue = time() + flow_ctrl_.calc_issue_busy_cycles(target_id, pld);
    }
}
