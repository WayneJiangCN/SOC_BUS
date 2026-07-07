#include "tm_mesh.h"

#include <unordered_set>

using namespace std;
using namespace tm_engine;

namespace
{

/* 响应回程同样需要防止同拍多次前推。 */
uintptr_t
mesh_rsp_packet_tag(p_tm_pld_t pld)
{
    return reinterpret_cast<uintptr_t>(pld.get());
}

}  // namespace

void
TmMeshFabric::recv_target_rsps()
{
    /* 从所有 target 接口吸收回包，并注入 mesh 回程子网。 */
    for (uint32_t target = 0; target < cfg_->num_targets; ++target) {
        recv_target_wr_req_rsp(target);
        recv_target_wr_dat_rsp(target);
        for (uint32_t lane = 0; lane < cfg_->rd_rsp_port_num; ++lane) {
            recv_target_rd_rsp(target, lane);
        }
    }
}

void
TmMeshFabric::recv_target_rd_rsp(uint32_t target_id, uint32_t lane)
{
    /* 读响应从目标 router 进入 mesh 回程网络。 */
    uint32_t chan = static_cast<uint32_t>(aic_req_type_t::RD_REQ) + lane;
    auto inf = v_target_inf_[target_id];
    auto router_fifo =
        get_mesh_rd_rsp_fifo(topology_.target_node(target_id), lane);

    while (inf->valid(chan) && !router_fifo->full()) {
        auto rsp = inf->get_pld(chan);

        auto key = make_txn_key(rsp);
        auto ctx_it = txn_ctx_.find(key);
        if (ctx_it == txn_ctx_.end()) {
            break;
        }

        router_fifo->push_back(rsp);
        inf->pop_pld(chan);
        ctx_it->second.state = tm_mesh_txn_state_t::IN_RSP_MESH;
    }
}

void
TmMeshFabric::recv_target_wr_req_rsp(uint32_t target_id)
{
    /* WR_REQ_RSP 同时承担响应返回与 grant 生成两项职责。 */
    uint32_t chan = static_cast<uint32_t>(aic_req_type_t::WR_REQ);
    auto inf = v_target_inf_[target_id];
    auto router_fifo =
        get_mesh_wr_req_rsp_fifo(topology_.target_node(target_id));

    while (inf->valid(chan) && !router_fifo->full()) {
        auto rsp = inf->get_pld(chan);

        auto key = make_txn_key(rsp);
        auto ctx_it = txn_ctx_.find(key);
        if (ctx_it == txn_ctx_.end()) {
            break;
        }

        uint32_t master_port = topology_.find_master_port(rsp->mst_id);
        if (m_wr_grant_fifo_[master_port].size() >=
            cfg_->master_wr_grant_fifo_depth) {
            break;
        }

        TmMeshGrant grant;
        grant.gid = rsp->gid;
        grant.target_id = target_id;
        grant.chan = rsp->chan;
        grant.dbid = rsp->tnx_id;
        m_wr_grant_fifo_[master_port].push_back(grant);

        router_fifo->push_back(rsp);
        inf->pop_pld(chan);
        ctx_it->second.state = tm_mesh_txn_state_t::IN_RSP_MESH;
    }
}

void
TmMeshFabric::recv_target_wr_dat_rsp(uint32_t target_id)
{
    uint32_t chan = static_cast<uint32_t>(aic_req_type_t::WR_DAT);
    auto inf = v_target_inf_[target_id];
    auto router_fifo =
        get_mesh_wr_dat_rsp_fifo(topology_.target_node(target_id));

    while (inf->valid(chan) && !router_fifo->full()) {
        auto rsp = inf->get_pld(chan);

        auto key = make_txn_key(rsp);
        auto ctx_it = txn_ctx_.find(key);
        if (ctx_it == txn_ctx_.end()) {
            break;
        }

        router_fifo->push_back(rsp);
        inf->pop_pld(chan);
        ctx_it->second.state = tm_mesh_txn_state_t::IN_RSP_MESH;
    }
}

void
TmMeshFabric::advance_mesh_rsps()
{
    advance_mesh_wr_req_rsps();
    advance_mesh_wr_dat_rsps();
    advance_mesh_rd_rsps();
}

void
TmMeshFabric::advance_mesh_rd_rsps()
{
    /* 读响应按 lane 分离推进，避免不同 lane 相互阻塞。 */
    std::unordered_set<uintptr_t> moved_tags;

    for (uint32_t router = 0; router < mesh_router_count_; ++router) {
        bool moved_this_router = false;
        for (uint32_t lane = 0; lane < cfg_->rd_rsp_port_num; ++lane) {
            auto fifo = get_mesh_rd_rsp_fifo(router, lane);
            if (fifo->empty()) {
                continue;
            }

            auto rsp = fifo->front();
            uintptr_t tag = mesh_rsp_packet_tag(rsp);
            if (moved_tags.find(tag) != moved_tags.end()) {
                continue;
            }

            auto key = make_txn_key(rsp);
            auto ctx_it = txn_ctx_.find(key);
            if (ctx_it == txn_ctx_.end()) {
                continue;
            }

            uint32_t dst_router = ctx_it->second.src_node;
            uint32_t master_port = ctx_it->second.master_port;
            if (router == dst_router) {
                auto master_fifo = m_rd_rsp_fifo_[master_port][lane];
                if (master_fifo->full()) {
                    continue;
                }

                fifo->pop_front();
                master_fifo->push_back(rsp);
                moved_tags.insert(tag);
                moved_this_router = true;
                break;
            }

            if (time() < next_mesh_rd_rsp_hop_time_[router]) {
                continue;
            }

            uint32_t next_router =
                topology_.compute_next_node(router, dst_router);
            auto next_fifo = get_mesh_rd_rsp_fifo(next_router, lane);
            if (next_fifo->full()) {
                continue;
            }

            fifo->pop_front();
            next_fifo->push_back(rsp);
            next_mesh_rd_rsp_hop_time_[router] = time() + mesh_link_latency_;
            moved_tags.insert(tag);
            moved_this_router = true;
            break;
        }

        if (moved_this_router) {
            continue;
        }
    }
}

void
TmMeshFabric::advance_mesh_wr_req_rsps()
{
    /* 写请求响应回到源 router 后，才落入 master 的回包 FIFO。 */
    std::unordered_set<uintptr_t> moved_tags;

    for (uint32_t router = 0; router < mesh_router_count_; ++router) {
        auto fifo = get_mesh_wr_req_rsp_fifo(router);
        if (fifo->empty()) {
            continue;
        }

        auto rsp = fifo->front();
        uintptr_t tag = mesh_rsp_packet_tag(rsp);
        if (moved_tags.find(tag) != moved_tags.end()) {
            continue;
        }

        auto key = make_txn_key(rsp);
        auto ctx_it = txn_ctx_.find(key);
        if (ctx_it == txn_ctx_.end()) {
            continue;
        }

        uint32_t dst_router = ctx_it->second.src_node;
        uint32_t master_port = ctx_it->second.master_port;
        if (router == dst_router) {
            auto master_fifo = m_wr_req_rsp_fifo_[master_port];
            if (master_fifo->full()) {
                continue;
            }

            fifo->pop_front();
            master_fifo->push_back(rsp);
            moved_tags.insert(tag);
            continue;
        }

        if (time() < next_mesh_wr_req_rsp_hop_time_[router]) {
            continue;
        }

        uint32_t next_router =
            topology_.compute_next_node(router, dst_router);
        auto next_fifo = get_mesh_wr_req_rsp_fifo(next_router);
        if (next_fifo->full()) {
            continue;
        }

        fifo->pop_front();
        next_fifo->push_back(rsp);
        next_mesh_wr_req_rsp_hop_time_[router] = time() + mesh_link_latency_;
        moved_tags.insert(tag);
    }
}

void
TmMeshFabric::advance_mesh_wr_dat_rsps()
{
    /* 写完成响应回到源 router 后，写事务生命周期结束。 */
    std::unordered_set<uintptr_t> moved_tags;

    for (uint32_t router = 0; router < mesh_router_count_; ++router) {
        auto fifo = get_mesh_wr_dat_rsp_fifo(router);
        if (fifo->empty()) {
            continue;
        }

        auto rsp = fifo->front();
        uintptr_t tag = mesh_rsp_packet_tag(rsp);
        if (moved_tags.find(tag) != moved_tags.end()) {
            continue;
        }

        auto key = make_txn_key(rsp);
        auto ctx_it = txn_ctx_.find(key);
        if (ctx_it == txn_ctx_.end()) {
            continue;
        }

        uint32_t dst_router = ctx_it->second.src_node;
        uint32_t master_port = ctx_it->second.master_port;
        if (router == dst_router) {
            auto master_fifo = m_wr_dat_rsp_fifo_[master_port];
            if (master_fifo->full()) {
                continue;
            }

            fifo->pop_front();
            master_fifo->push_back(rsp);
            moved_tags.insert(tag);
            continue;
        }

        if (time() < next_mesh_wr_dat_rsp_hop_time_[router]) {
            continue;
        }

        uint32_t next_router =
            topology_.compute_next_node(router, dst_router);
        auto next_fifo = get_mesh_wr_dat_rsp_fifo(next_router);
        if (next_fifo->full()) {
            continue;
        }

        fifo->pop_front();
        next_fifo->push_back(rsp);
        next_mesh_wr_dat_rsp_hop_time_[router] = time() + mesh_link_latency_;
        moved_tags.insert(tag);
    }
}

void
TmMeshFabric::send_master_rsps()
{
    for (uint32_t master = 0; master < cfg_->num_masters; ++master) {
        send_master_wr_req_rsp(master);
        send_master_wr_dat_rsp(master);
        for (uint32_t lane = 0; lane < cfg_->rd_rsp_port_num; ++lane) {
            send_master_rd_rsp(master, lane);
        }
    }
}

void
TmMeshFabric::send_master_rd_rsp(uint32_t master_port, uint32_t lane)
{
    /* 送回读响应时，同时做 credit 释放和多拍响应计数。 */
    auto fifo = m_rd_rsp_fifo_[master_port][lane];
    if (fifo->empty()) {
        return;
    }

    auto rsp = fifo->front();
    auto key = make_txn_key(rsp);
    auto ctx_it = txn_ctx_.find(key);
    if (ctx_it == txn_ctx_.end()) {
        return;
    }
    uint32_t target_id = ctx_it->second.target_id;
    if (time() < next_rd_rsp_issue_time_[master_port][lane]) {
        return;
    }

    uint32_t chan = static_cast<uint32_t>(aic_req_type_t::RD_REQ) + lane;
    if (v_master_inf_[master_port]->send(chan, rsp)) {
        fifo->pop_front();
        next_rd_rsp_issue_time_[master_port][lane] =
            time() + flow_ctrl_.calc_rsp_busy_cycles(target_id, rsp);

        if (ctx_it->second.rsp_expected == 1 && rsp->latency > 1) {
            ctx_it->second.rsp_expected = rsp->latency;
        }
        ctx_it->second.rsp_seen++;
        if (!ctx_it->second.slot_released) {
            flow_ctrl_.release_target_credit(target_id, aic_req_type_t::RD_REQ);
            ctx_it->second.slot_released = true;
        }
        if (ctx_it->second.rsp_seen >= ctx_it->second.rsp_expected) {
            ctx_it->second.state = tm_mesh_txn_state_t::DONE;
            txn_ctx_.erase(ctx_it);
        }
    }
}

void
TmMeshFabric::send_master_wr_req_rsp(uint32_t master_port)
{
    /* WR_REQ_RSP 回到 BIU 后，BIU 才能继续 WR_DAT。 */
    auto fifo = m_wr_req_rsp_fifo_[master_port];
    if (fifo->empty()) {
        return;
    }

    auto rsp = fifo->front();
    auto key = make_txn_key(rsp);
    auto ctx_it = txn_ctx_.find(key);
    if (ctx_it == txn_ctx_.end()) {
        return;
    }
    uint32_t target_id = ctx_it->second.target_id;
    if (time() < next_wr_req_rsp_issue_time_[master_port]) {
        return;
    }

    if (v_master_inf_[master_port]->send(
            static_cast<uint32_t>(aic_req_type_t::WR_REQ), rsp)) {
        fifo->pop_front();
        next_wr_req_rsp_issue_time_[master_port] =
            time() + flow_ctrl_.calc_rsp_busy_cycles(target_id, rsp);
    }
}

void
TmMeshFabric::send_master_wr_dat_rsp(uint32_t master_port)
{
    /* WR_DAT_RSP 是写事务最终完成点，并在这里释放写 slot。 */
    auto fifo = m_wr_dat_rsp_fifo_[master_port];
    if (fifo->empty()) {
        return;
    }

    auto rsp = fifo->front();
    auto key = make_txn_key(rsp);
    auto ctx_it = txn_ctx_.find(key);
    if (ctx_it == txn_ctx_.end()) {
        return;
    }
    uint32_t target_id = ctx_it->second.target_id;
    if (time() < next_wr_dat_rsp_issue_time_[master_port]) {
        return;
    }

    if (v_master_inf_[master_port]->send(
            static_cast<uint32_t>(aic_req_type_t::WR_DAT), rsp)) {
        fifo->pop_front();
        next_wr_dat_rsp_issue_time_[master_port] =
            time() + flow_ctrl_.calc_rsp_busy_cycles(target_id, rsp);
        flow_ctrl_.release_target_credit(target_id, aic_req_type_t::WR_DAT);
        ctx_it->second.state = tm_mesh_txn_state_t::DONE;
        txn_ctx_.erase(ctx_it);
    }
}
