#include "tm_mesh.h"

#include "tm_mesh_target_port.h"

using namespace std;
using namespace tm_engine;

void
TmMeshFabric::recv_target_rsps()
{
    // target 侧响应统一先注入目标 router 的 LOCAL 输入口，
    // 后续再由 advance_mesh_routers() 负责回程前推。
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
    // 读响应按 lane 分开进入本地 RD_RSP 队列。
    uint32_t chan = static_cast<uint32_t>(aic_req_type_t::RD_REQ) + lane;
    auto target_port = target_ports_[target_id];
    auto inf = target_port->inf();
    auto router_fifo = get_mesh_rd_rsp_fifo(topology_.target_node(target_id),
                                            TmMeshPortDir::LOCAL, lane);

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
    // WR_REQ_RSP 是写事务 grant 返回路径的起点。
    uint32_t chan = static_cast<uint32_t>(aic_req_type_t::WR_REQ);
    auto target_port = target_ports_[target_id];
    auto inf = target_port->inf();
    auto router_fifo = get_mesh_wr_req_rsp_fifo(topology_.target_node(target_id),
                                                TmMeshPortDir::LOCAL);

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
TmMeshFabric::recv_target_wr_dat_rsp(uint32_t target_id)
{
    // WR_DAT_RSP 是写事务最终完成响应。
    uint32_t chan = static_cast<uint32_t>(aic_req_type_t::WR_DAT);
    auto target_port = target_ports_[target_id];
    auto inf = target_port->inf();
    auto router_fifo = get_mesh_wr_dat_rsp_fifo(topology_.target_node(target_id),
                                                TmMeshPortDir::LOCAL);

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
