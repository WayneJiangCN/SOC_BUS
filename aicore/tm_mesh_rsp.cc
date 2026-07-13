#include "tm_mesh.h"

#include "tm_mesh_target_port.h"

using namespace std;
using namespace tm_engine;

void
TmMeshFabric::recv_target_rsps()
{
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
    auto target_port = target_ports_[target_id];
    auto router_fifo = get_mesh_rd_rsp_fifo(topology_->target_node(target_id),
                                            TmMeshPortDir::LOCAL, lane);

    while (target_port->has_response(aic_req_type_t::RD_REQ, lane) &&
           !router_fifo->full()) {
        auto rsp = target_port->front_response(aic_req_type_t::RD_REQ, lane);

        router_fifo->push_back(rsp);
        target_port->pop_response(aic_req_type_t::RD_REQ, lane);
    }
}

void
TmMeshFabric::recv_target_wr_req_rsp(uint32_t target_id)
{
    auto target_port = target_ports_[target_id];
    auto router_fifo = get_mesh_wr_req_rsp_fifo(
        topology_->target_node(target_id), TmMeshPortDir::LOCAL);

    while (target_port->has_response(aic_req_type_t::WR_REQ) &&
           !router_fifo->full()) {
        auto rsp = target_port->front_response(aic_req_type_t::WR_REQ);

        router_fifo->push_back(rsp);
        target_port->pop_response(aic_req_type_t::WR_REQ);
    }
}

void
TmMeshFabric::recv_target_wr_dat_rsp(uint32_t target_id)
{
    auto target_port = target_ports_[target_id];
    auto router_fifo = get_mesh_wr_dat_rsp_fifo(
        topology_->target_node(target_id), TmMeshPortDir::LOCAL);

    while (target_port->has_response(aic_req_type_t::WR_DAT) &&
           !router_fifo->full()) {
        auto rsp = target_port->front_response(aic_req_type_t::WR_DAT);

        router_fifo->push_back(rsp);
        target_port->pop_response(aic_req_type_t::WR_DAT);
    }
}
