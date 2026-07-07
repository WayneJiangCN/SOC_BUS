#include "tm_bus.h"

#include <unordered_set>

using namespace std;
using namespace tm_engine;

namespace
{

/* 用 pld 指针地址标记“本拍是否已经移动过”，避免同拍连跳两次。 */
uintptr_t
packet_tag(p_tm_pld_t pld)
{
    return reinterpret_cast<uintptr_t>(pld.get());
}

}  // namespace

void
TmBusFabric::recv_master_reqs()
{
    /* 从所有 master 接口收包，并按事务类型拆入入口 FIFO。 */
    for (uint32_t master = 0; master < cfg_->num_masters; ++master) {
        recv_master_req(master, aic_req_type_t::WR_REQ);
        recv_master_req(master, aic_req_type_t::WR_DAT);
        recv_master_req(master, aic_req_type_t::RD_REQ);
    }
}

void
TmBusFabric::recv_master_req(uint32_t master_port, aic_req_type_t req_type)
{
    /* 吸收一个 master 通道上的请求，同时建立或更新事务上下文。 */
    uint32_t chan = static_cast<uint32_t>(req_type);
    auto inf = v_master_inf_[master_port];
    auto fifo = get_master_fifo(master_port, req_type);

    while (inf->valid(chan) && !fifo->full()) {
        auto pld = inf->get_pld(chan);

        uint32_t expected_mst_id = topology_.port_master_id(master_port);
        if (pld->mst_id == 0) {
            pld->mst_id = expected_mst_id;
        } else {
        }

        auto key = make_txn_key(pld);
        auto it = txn_ctx_.find(key);
        if (req_type == aic_req_type_t::WR_DAT) {
            /* WR_DAT 复用同一笔写事务前面 WR_REQ 创建的上下文。 */
            it->second.state = tm_bus_txn_state_t::IN_INGRESS_FIFO;
        } else {
            TmBusTxnCtx ctx;
            ctx.master_port = master_port;
            ctx.target_id = topology_.decode_target(pld->addr);
            ctx.src_node = topology_.master_node(master_port);
            ctx.dst_node = topology_.target_node(ctx.target_id);
            ctx.req_type = req_type;
            ctx.state = tm_bus_txn_state_t::IN_INGRESS_FIFO;
            ctx.size = pld->size;
            ctx.issue_time = time();
            auto ret = txn_ctx_.emplace(key, ctx);
        }

        fifo->push_back(pld);
        inf->pop_pld(chan);
    }
}

void
TmBusFabric::inject_ring_reqs()
{
    for (uint32_t master = 0; master < cfg_->num_masters; ++master) {
        inject_ring_req(master, aic_req_type_t::RD_REQ);
        inject_ring_req(master, aic_req_type_t::WR_REQ);
        inject_ring_req(master, aic_req_type_t::WR_DAT);
    }
}

void
TmBusFabric::inject_ring_req(uint32_t master_port, aic_req_type_t req_type)
{
    /* master ingress FIFO -> source ring node FIFO。 */
    auto master_fifo = get_master_fifo(master_port, req_type);
    auto node_id = topology_.master_node(master_port);
    auto ring_fifo = get_ring_req_fifo(node_id, req_type);

    while (!master_fifo->empty() && !ring_fifo->full()) {
        auto pld = master_fifo->front();

        auto key = make_txn_key(pld);
        auto ctx_it = txn_ctx_.find(key);

        if (req_type == aic_req_type_t::WR_DAT) {
            /* 写数据必须先等对应 grant 到队头，才能注入 ring。 */
            if (m_wr_grant_fifo_[master_port].empty()) {
                return;
            }
            const auto& grant = m_wr_grant_fifo_[master_port].front();
            if (!flow_ctrl_.wr_grant_match(grant.target_id, grant.gid,
                                           ctx_it->second.target_id, pld,
                                           cfg_->strict_wr_grant_order)) {
                return;
            }
        }

        master_fifo->pop_front();
        ring_fifo->push_back(pld);
        ctx_it->second.state = tm_bus_txn_state_t::IN_REQ_RING;
    }
}

void
TmBusFabric::advance_ring_reqs()
{
    advance_ring_req_type(aic_req_type_t::RD_REQ);
    advance_ring_req_type(aic_req_type_t::WR_REQ);
    advance_ring_req_type(aic_req_type_t::WR_DAT);
}

void
TmBusFabric::advance_ring_req_type(aic_req_type_t req_type)
{
    /*
     * ring 每拍最多单跳前推：
     * - 到达目的节点就落到 target local FIFO
     * - 否则转发到 next_ring_node()
     */
    std::unordered_set<uintptr_t> moved_tags;
    std::vector<tm_time_t>* next_hop = nullptr;
    if (req_type == aic_req_type_t::RD_REQ) {
        next_hop = &next_n_rd_req_hop_time_;
    } else if (req_type == aic_req_type_t::WR_REQ) {
        next_hop = &next_n_wr_req_hop_time_;
    } else {
        next_hop = &next_n_wr_dat_hop_time_;
    }

    for (uint32_t node = 0; node < ring_node_count_; ++node) {
        auto fifo = get_ring_req_fifo(node, req_type);
        if (fifo->empty()) {
            continue;
        }

        auto pld = fifo->front();
        uintptr_t tag = packet_tag(pld);
        if (moved_tags.find(tag) != moved_tags.end()) {
            continue;
        }

        auto key = make_txn_key(pld);
        auto ctx_it = txn_ctx_.find(key);

        if (node == ctx_it->second.dst_node) {
            /* 命中目标节点后，不再继续占用 ring。 */
            auto target_fifo = get_target_fifo(ctx_it->second.target_id, req_type);
            if (target_fifo->full()) {
                continue;
            }

            fifo->pop_front();
            target_fifo->push_back(pld);
            ctx_it->second.state = tm_bus_txn_state_t::IN_TARGET_FIFO;
            moved_tags.insert(tag);
            continue;
        }

        if (time() < (*next_hop)[node]) {
            continue;
        }

        uint32_t next_node = topology_.next_ring_node(node);
        auto next_fifo = get_ring_req_fifo(next_node, req_type);
        if (next_fifo->full()) {
            continue;
        }

        fifo->pop_front();
        next_fifo->push_back(pld);
        (*next_hop)[node] = time() + ring_link_latency_;
        ctx_it->second.state = tm_bus_txn_state_t::IN_REQ_RING;
        moved_tags.insert(tag);
    }
}

void
TmBusFabric::send_target_reqs()
{
    for (uint32_t target = 0; target < cfg_->num_targets; ++target) {
        send_target_req(target, aic_req_type_t::RD_REQ);
        send_target_req(target, aic_req_type_t::WR_REQ);
        send_target_req(target, aic_req_type_t::WR_DAT);
    }
}

void
TmBusFabric::send_target_req(uint32_t target_id, aic_req_type_t req_type)
{
    /* target local FIFO -> target 接口，正式受 target credit/token 约束。 */
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
        if (req_type == aic_req_type_t::RD_REQ) {
            ctx_it->second.state = tm_bus_txn_state_t::WAIT_RD_RSP;
        } else if (req_type == aic_req_type_t::WR_REQ) {
            ctx_it->second.state = tm_bus_txn_state_t::WAIT_WR_REQ_RSP;
        } else {
            ctx_it->second.state = tm_bus_txn_state_t::WAIT_WR_DAT_RSP;
        }

        if (req_type == aic_req_type_t::WR_DAT) {
            /* 写数据真正发出后，grant 才被消耗掉。 */
            uint32_t master_port = topology_.find_master_port(pld->mst_id);
            m_wr_grant_fifo_[master_port].pop_front();
        }

        *next_issue = time() + flow_ctrl_.calc_issue_busy_cycles(target_id, pld);
    }
}
