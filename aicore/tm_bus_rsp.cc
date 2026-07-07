#include "tm_bus.h"

#include <unordered_set>

using namespace std;
using namespace tm_engine;

namespace
{

/* 响应路径也用 pld 指针地址标记本拍移动，避免同拍多次回推。 */
uintptr_t
packet_tag(p_tm_pld_t pld)
{
    return reinterpret_cast<uintptr_t>(pld.get());
}

}  // namespace

void
TmBusFabric::recv_target_rsps()
{
    /* 从所有 target 收响应，并先注入 ring 回程网络。 */
    for (uint32_t target = 0; target < cfg_->num_targets; ++target) {
        recv_target_wr_req_rsp(target);
        recv_target_wr_dat_rsp(target);
        for (uint32_t lane = 0; lane < cfg_->rd_rsp_port_num; ++lane) {
            recv_target_rd_rsp(target, lane);
        }
    }
}

void
TmBusFabric::recv_target_rd_rsp(uint32_t target_id, uint32_t lane)
{
    /* 读响应从目标节点进入 ring 回程子网。 */
    uint32_t chan = static_cast<uint32_t>(aic_req_type_t::RD_REQ) + lane;
    auto inf = v_target_inf_[target_id];
    auto node_fifo = get_ring_rd_rsp_fifo(topology_.target_node(target_id), lane);

    while (inf->valid(chan) && !node_fifo->full()) {
        auto rsp = inf->get_pld(chan);

        auto key = make_txn_key(rsp);
        auto ctx_it = txn_ctx_.find(key);

        node_fifo->push_back(rsp);
        inf->pop_pld(chan);
        ctx_it->second.state = tm_bus_txn_state_t::IN_RSP_RING;
    }
}

void
TmBusFabric::recv_target_wr_req_rsp(uint32_t target_id)
{
    /*
     * WR_REQ_RSP 有两层含义：
     * 1. 它本身是回给 master 的响应
     * 2. 它还会生成后续 WR_DAT 所需的 grant
     */
    uint32_t chan = static_cast<uint32_t>(aic_req_type_t::WR_REQ);
    auto inf = v_target_inf_[target_id];
    auto node_fifo = get_ring_wr_req_rsp_fifo(topology_.target_node(target_id));

    while (inf->valid(chan) && !node_fifo->full()) {
        auto rsp = inf->get_pld(chan);

        auto key = make_txn_key(rsp);
        auto ctx_it = txn_ctx_.find(key);

        uint32_t master_port = topology_.find_master_port(rsp->mst_id);
        if (m_wr_grant_fifo_[master_port].size() >=
            cfg_->master_wr_grant_fifo_depth) {
            break;
        }

        TmBusGrant grant;
        grant.gid = rsp->gid;
        grant.target_id = target_id;
        grant.chan = rsp->chan;
        grant.dbid = rsp->tnx_id;
        m_wr_grant_fifo_[master_port].push_back(grant);

        node_fifo->push_back(rsp);
        inf->pop_pld(chan);
        ctx_it->second.state = tm_bus_txn_state_t::IN_RSP_RING;
    }
}

void
TmBusFabric::recv_target_wr_dat_rsp(uint32_t target_id)
{

    uint32_t chan = static_cast<uint32_t>(aic_req_type_t::WR_DAT);
    auto inf = v_target_inf_[target_id];
    auto node_fifo = get_ring_wr_dat_rsp_fifo(topology_.target_node(target_id));

    while (inf->valid(chan) && !node_fifo->full()) {
        auto rsp = inf->get_pld(chan);

        auto key = make_txn_key(rsp);
        auto ctx_it = txn_ctx_.find(key);

        node_fifo->push_back(rsp);
        inf->pop_pld(chan);
        ctx_it->second.state = tm_bus_txn_state_t::IN_RSP_RING;
    }
}

void
TmBusFabric::advance_ring_rsps()
{
    advance_ring_wr_req_rsps();
    advance_ring_wr_dat_rsps();
    advance_ring_rd_rsps();
}

void
TmBusFabric::advance_ring_rd_rsps()
{
    /* 按 lane 分开推进读响应，避免多 lane 互相影响。 */
    std::unordered_set<uintptr_t> moved_tags;

    for (uint32_t node = 0; node < ring_node_count_; ++node) {
        bool moved_this_node = false;
        for (uint32_t lane = 0; lane < cfg_->rd_rsp_port_num; ++lane) {
            auto fifo = get_ring_rd_rsp_fifo(node, lane);
            if (fifo->empty()) {
                continue;
            }

            auto rsp = fifo->front();
            uintptr_t tag = packet_tag(rsp);
            if (moved_tags.find(tag) != moved_tags.end()) {
                continue;
            }

            auto key = make_txn_key(rsp);
            auto ctx_it = txn_ctx_.find(key);

            uint32_t dst_node = ctx_it->second.src_node;
            uint32_t master_port = ctx_it->second.master_port;
            if (node == dst_node) {
                auto master_fifo = m_rd_rsp_fifo_[master_port][lane];
                if (master_fifo->full()) {
                    continue;
                }

                fifo->pop_front();
                master_fifo->push_back(rsp);
                moved_tags.insert(tag);
                moved_this_node = true;
                break;
            }

            if (time() < next_n_rd_rsp_hop_time_[node]) {
                continue;
            }

            uint32_t next_node = topology_.next_ring_node(node);
            auto next_fifo = get_ring_rd_rsp_fifo(next_node, lane);
            if (next_fifo->full()) {
                continue;
            }

            fifo->pop_front();
            next_fifo->push_back(rsp);
            next_n_rd_rsp_hop_time_[node] = time() + ring_link_latency_;
            moved_tags.insert(tag);
            moved_this_node = true;
            break;
        }

        if (moved_this_node) {
            continue;
        }
    }
}

void
TmBusFabric::advance_ring_wr_req_rsps()
{
    /* 写请求响应回到源节点后，进入 master 的 WR_REQ_RSP FIFO。 */
    std::unordered_set<uintptr_t> moved_tags;

    for (uint32_t node = 0; node < ring_node_count_; ++node) {
        auto fifo = get_ring_wr_req_rsp_fifo(node);
        if (fifo->empty()) {
            continue;
        }

        auto rsp = fifo->front();
        uintptr_t tag = packet_tag(rsp);
        if (moved_tags.find(tag) != moved_tags.end()) {
            continue;
        }

        auto key = make_txn_key(rsp);
        auto ctx_it = txn_ctx_.find(key);

        uint32_t dst_node = ctx_it->second.src_node;
        uint32_t master_port = ctx_it->second.master_port;
        if (node == dst_node) {
            auto master_fifo = m_wr_req_rsp_fifo_[master_port];
            if (master_fifo->full()) {
                continue;
            }

            fifo->pop_front();
            master_fifo->push_back(rsp);
            moved_tags.insert(tag);
            continue;
        }

        if (time() < next_n_wr_req_rsp_hop_time_[node]) {
            continue;
        }

        uint32_t next_node = topology_.next_ring_node(node);
        auto next_fifo = get_ring_wr_req_rsp_fifo(next_node);
        if (next_fifo->full()) {
            continue;
        }

        fifo->pop_front();
        next_fifo->push_back(rsp);
        next_n_wr_req_rsp_hop_time_[node] = time() + ring_link_latency_;
        moved_tags.insert(tag);
    }
}

void
TmBusFabric::advance_ring_wr_dat_rsps()
{
    /* 写完成响应回到源节点后，写事务才真正结束。 */
    std::unordered_set<uintptr_t> moved_tags;

    for (uint32_t node = 0; node < ring_node_count_; ++node) {
        auto fifo = get_ring_wr_dat_rsp_fifo(node);
        if (fifo->empty()) {
            continue;
        }

        auto rsp = fifo->front();
        uintptr_t tag = packet_tag(rsp);
        if (moved_tags.find(tag) != moved_tags.end()) {
            continue;
        }

        auto key = make_txn_key(rsp);
        auto ctx_it = txn_ctx_.find(key);

        uint32_t dst_node = ctx_it->second.src_node;
        uint32_t master_port = ctx_it->second.master_port;
        if (node == dst_node) {
            auto master_fifo = m_wr_dat_rsp_fifo_[master_port];
            if (master_fifo->full()) {
                continue;
            }

            fifo->pop_front();
            master_fifo->push_back(rsp);
            moved_tags.insert(tag);
            continue;
        }

        if (time() < next_n_wr_dat_rsp_hop_time_[node]) {
            continue;
        }

        uint32_t next_node = topology_.next_ring_node(node);
        auto next_fifo = get_ring_wr_dat_rsp_fifo(next_node);
        if (next_fifo->full()) {
            continue;
        }

        fifo->pop_front();
        next_fifo->push_back(rsp);
        next_n_wr_dat_rsp_hop_time_[node] = time() + ring_link_latency_;
        moved_tags.insert(tag);
    }
}

void
TmBusFabric::send_master_rsps()
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
TmBusFabric::send_master_rd_rsp(uint32_t master_port, uint32_t lane)
{
    /*
     * 送回读响应时，同时：
     * 1. 更新多拍响应计数
     * 2. 释放 target 侧读 slot
     */
    auto fifo = m_rd_rsp_fifo_[master_port][lane];
    if (fifo->empty()) {
        return;
    }

    auto rsp = fifo->front();

    auto key = make_txn_key(rsp);
    auto ctx_it = txn_ctx_.find(key);
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
            ctx_it->second.state = tm_bus_txn_state_t::DONE;
            txn_ctx_.erase(ctx_it);
        }
    }
}

void
TmBusFabric::send_master_wr_req_rsp(uint32_t master_port)
{
    /* WR_REQ_RSP 回到 BIU 后，BIU 才能继续组织 WR_DAT。 */
    auto fifo = m_wr_req_rsp_fifo_[master_port];
    if (fifo->empty()) {
        return;
    }

    auto rsp = fifo->front();

    auto key = make_txn_key(rsp);
    auto ctx_it = txn_ctx_.find(key);
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
TmBusFabric::send_master_wr_dat_rsp(uint32_t master_port)
{
    /* WR_DAT_RSP 是写事务的最终完成点，并在这里释放写 slot。 */
    auto fifo = m_wr_dat_rsp_fifo_[master_port];
    if (fifo->empty()) {
        return;
    }

    auto rsp = fifo->front();

    auto key = make_txn_key(rsp);
    auto ctx_it = txn_ctx_.find(key);
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
        ctx_it->second.state = tm_bus_txn_state_t::DONE;
        txn_ctx_.erase(ctx_it);
    }
}
