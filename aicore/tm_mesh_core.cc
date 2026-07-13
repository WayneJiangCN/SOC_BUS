#include <algorithm>

#include "pem_biu.h"
#include "tm_mesh.h"
#include "tm_mesh_inf.h"
#include "tm_mesh_link.h"
#include "tm_mesh_pld.h"
#include "tm_mesh_router.h"
#include "tm_mesh_target_port.h"

using namespace std;
using namespace tm_engine;

TmMeshFabric::TmMeshFabric()
{
}

TmMeshFabric::TmMeshFabric(p_tm_clk_t clk, p_tm_mesh_cfg_t cfg)
    : clk_(clk), cfg_(cfg)
{
    this->name(cfg_->name);
    config();
}

TmMeshFabric::~TmMeshFabric()
{
}

void
TmMeshFabric::config()
{
    topology_ = std::make_shared<TmMeshTopology>();
    flow_ctrl_ = std::make_shared<TmBusFlowCtrl>();

    topology_->config(cfg_);

    auto flow_ctrl_cfg = std::make_shared<tm_bus_cfg_t>();
    flow_ctrl_cfg->num_targets = cfg_->num_targets;
    flow_ctrl_cfg->targets = cfg_->targets;
    flow_ctrl_->config(flow_ctrl_cfg);

    mesh_router_count_ = topology_->router_count();
    mesh_rows_ = topology_->rows();
    mesh_cols_ = topology_->cols();
    mesh_link_latency_ = cfg_->mesh_link_latency;

    master_nius_.clear();
    routers_.clear();
    links_.clear();
    target_ports_.clear();

    next_rd_issue_time_.assign(cfg_->num_targets, 0);
    next_wr_req_issue_time_.assign(cfg_->num_targets, 0);
    next_wr_dat_issue_time_.assign(cfg_->num_targets, 0);

    topology_->reset(cfg_->num_masters);

    uint32_t token_period = 0;
    for (const auto& target_cfg : cfg_->targets) {
        if (target_cfg != nullptr && target_cfg->token_update_period > 0) {
            if (token_period == 0) {
                token_period = target_cfg->token_update_period;
            } else {
                token_period = std::min(token_period,
                                        target_cfg->token_update_period);
            }
        }
    }
    if (token_period > 0) {
        token_clk_ = tm_make_clk(this->name() + "_token_clk", token_period);
    }

    for (uint32_t i = 0; i < cfg_->num_masters; ++i) {
        master_nius_.push_back(tm_make_mesh_inf(
            this->name() + "_master_niu" + std::to_string(i), clk_, i, cfg_));
    }

    for (uint32_t i = 0; i < cfg_->num_targets; ++i) {
        auto target_cfg = cfg_->targets[i];
        target_ports_.push_back(tm_make_mesh_target_port(
            this->name() + "_target_port_" + std::to_string(i), clk_,
            target_cfg, cfg_->rd_rsp_port_num, cfg_->target_inf_depth));
    }

    for (uint32_t router = 0; router < mesh_router_count_; ++router) {
        routers_.push_back(tm_make_mesh_router(
            this->name() + "_router_" + std::to_string(router), clk_, cfg_));
    }

    for (uint32_t router = 0; router < mesh_router_count_; ++router) {
        if (topology_->has_neighbor(router, TmMeshPortDir::EAST)) {
            uint32_t east = topology_->neighbor(router, TmMeshPortDir::EAST);
            links_[make_link_key(router, TmMeshPortDir::EAST, east,
                                 TmMeshPortDir::WEST)] =
                tm_make_mesh_link(this->name() + "_link_" +
                                      std::to_string(router) + "_E_" +
                                      std::to_string(east) + "_W",
                                  clk_, mesh_link_latency_, east,
                                  TmMeshPortDir::WEST);
        }
        if (topology_->has_neighbor(router, TmMeshPortDir::WEST)) {
            uint32_t west = topology_->neighbor(router, TmMeshPortDir::WEST);
            links_[make_link_key(router, TmMeshPortDir::WEST, west,
                                 TmMeshPortDir::EAST)] =
                tm_make_mesh_link(this->name() + "_link_" +
                                      std::to_string(router) + "_W_" +
                                      std::to_string(west) + "_E",
                                  clk_, mesh_link_latency_, west,
                                  TmMeshPortDir::EAST);
        }
        if (topology_->has_neighbor(router, TmMeshPortDir::SOUTH)) {
            uint32_t south = topology_->neighbor(router, TmMeshPortDir::SOUTH);
            links_[make_link_key(router, TmMeshPortDir::SOUTH, south,
                                 TmMeshPortDir::NORTH)] =
                tm_make_mesh_link(this->name() + "_link_" +
                                      std::to_string(router) + "_S_" +
                                      std::to_string(south) + "_N",
                                  clk_, mesh_link_latency_, south,
                                  TmMeshPortDir::NORTH);
        }
        if (topology_->has_neighbor(router, TmMeshPortDir::NORTH)) {
            uint32_t north = topology_->neighbor(router, TmMeshPortDir::NORTH);
            links_[make_link_key(router, TmMeshPortDir::NORTH, north,
                                 TmMeshPortDir::SOUTH)] =
                tm_make_mesh_link(this->name() + "_link_" +
                                      std::to_string(router) + "_N_" +
                                      std::to_string(north) + "_S",
                                  clk_, mesh_link_latency_, north,
                                  TmMeshPortDir::SOUTH);
        }
    }

    // Master NIU owns its local event bindings; fabric only provides context.
    for (uint32_t i = 0; i < master_nius_.size(); ++i) {
        const auto& niu = master_nius_[i];
        if (niu == nullptr) {
            continue;
        }
        uint32_t source_router = topology_->master_node(i);
        niu->attach(i, topology_, flow_ctrl_,
                    get_mesh_req_fifo(source_router, TmMeshPortDir::LOCAL,
                                      aic_req_type_t::RD_REQ),
                    get_mesh_req_fifo(source_router, TmMeshPortDir::LOCAL,
                                      aic_req_type_t::WR_DAT));
    }

    // Router queues own their local vld bindings and arbitration. Fabric only
    // supplies route/ready/commit callbacks.
    for (uint32_t router_id = 0; router_id < routers_.size(); ++router_id) {
        auto router = routers_[router_id];
        if (router == nullptr) {
            continue;
        }
        router->attach(
            router_id,
            [this](uint32_t id, TmMeshRouteCandidate& cand) {
                return resolve_candidate_route(id, cand);
            },
            [this](uint32_t id, const TmMeshRouteCandidate& cand) {
                return check_candidate_ready(id, cand);
            },
            [this](uint32_t id, const TmMeshRouteCandidate& cand) {
                return commit_router_candidate(id, cand);
            });
    }

    // Link queues own their local vld binding. Fabric only provides the target
    // FIFO lookup.
    for (const auto& it : links_) {
        auto link = it.second;
        if (link != nullptr) {
            link->attach(
                [this](uint32_t dst_router, TmMeshPortDir dst_dir,
                       uint32_t traffic_class, p_tm_pld_t pld) {
                    if (traffic_class == TmMeshRouter::REQ_CLASS) {
                        return get_mesh_req_fifo(dst_router, dst_dir,
                                                 tm_mesh_pld_req_type(pld));
                    }
                    if (traffic_class == TmMeshRouter::WR_DAT_CLASS) {
                        return get_mesh_req_fifo(dst_router, dst_dir,
                                                 aic_req_type_t::WR_DAT);
                    }
                    if (traffic_class == TmMeshRouter::WR_REQ_RSP_CLASS) {
                        return get_mesh_wr_req_rsp_fifo(dst_router, dst_dir);
                    }
                    if (traffic_class == TmMeshRouter::WR_DAT_RSP_CLASS) {
                        return get_mesh_wr_dat_rsp_fifo(dst_router, dst_dir);
                    }

                    uint32_t lane =
                        traffic_class - TmMeshRouter::RD_RSP_BASE_CLASS;
                    return get_mesh_rd_rsp_fifo(dst_router, dst_dir, lane);
                });
        }
    }

    // TargetPort owns target-side queue/response events.
    for (const auto& target_port : target_ports_) {
        if (target_port == nullptr) {
            continue;
        }
        target_port->attach([this]() { recv_target_rsps(); },
                            [this]() { send_target_reqs(); });
    }

    if (token_clk_ != nullptr) {
        tm_sensitive(TM_MAKE_CPROC(&TmMeshFabric::update_target_tokens),
                     token_clk_->pos_edge);
    }

    reset();
}

void
TmMeshFabric::build()
{
}

void
TmMeshFabric::reset()
{
    rd_rsp_states_.clear();

    for (auto& niu : master_nius_) {
        if (niu != nullptr) {
            niu->reset();
        }
    }
    for (auto& router : routers_) {
        if (router != nullptr) {
            router->reset();
        }
    }
    for (auto& it : links_) {
        if (it.second != nullptr) {
            it.second->reset();
        }
    }
    for (auto& target_port : target_ports_) {
        if (target_port != nullptr) {
            target_port->reset();
        }
    }

    std::fill(next_rd_issue_time_.begin(), next_rd_issue_time_.end(), 0);
    std::fill(next_wr_req_issue_time_.begin(), next_wr_req_issue_time_.end(), 0);
    std::fill(next_wr_dat_issue_time_.begin(), next_wr_dat_issue_time_.end(), 0);

    flow_ctrl_->reset();
}

bool
TmMeshFabric::idle()
{
    bool ret = rd_rsp_states_.empty();

    for (auto& niu : master_nius_) {
        ret = ret && niu != nullptr && niu->idle();
    }
    for (auto& router : routers_) {
        ret = ret && router != nullptr && router->idle();
    }
    for (auto& target_port : target_ports_) {
        ret = ret && target_port != nullptr && target_port->idle();
    }
    for (const auto& it : links_) {
        ret = ret && (it.second == nullptr || it.second->idle());
    }
    return ret;
}

void
TmMeshFabric::update_target_tokens()
{
    flow_ctrl_->update_tokens(time());
    send_target_reqs();
}

void
TmMeshFabric::attach_master(uint32_t idx, p_tm_mesh_inf_t inf)
{
    if (idx >= master_nius_.size() || inf == nullptr) {
        return;
    }

    inf->set_master_id(topology_->port_master_id(idx));
    master_nius_[idx] = inf;
    uint32_t source_router = topology_->master_node(idx);
    inf->attach(idx, topology_, flow_ctrl_,
                get_mesh_req_fifo(source_router, TmMeshPortDir::LOCAL,
                                  aic_req_type_t::RD_REQ),
                get_mesh_req_fifo(source_router, TmMeshPortDir::LOCAL,
                                  aic_req_type_t::WR_DAT));
}

void
TmMeshFabric::attach_master(p_tm_mesh_inf_t inf)
{
    if (inf == nullptr) {
        return;
    }
    attach_master(inf->inf_id_, inf);
}

void
TmMeshFabric::attach_master(uint32_t idx, p_tm_mesh_biu_t biu)
{
    if (biu == nullptr) {
        return;
    }
    attach_master(idx, biu->out_intf_);
}

void
TmMeshFabric::attach_master(uint32_t idx, p_tm_com_inf_t inf)
{
    if (idx >= master_nius_.size() || master_nius_[idx] == nullptr ||
        inf == nullptr) {
        return;
    }
    master_nius_[idx]->attach(inf);
}

void
TmMeshFabric::attach_target(uint32_t idx, p_tm_com_inf_t inf)
{
    if (idx >= target_ports_.size() || target_ports_[idx] == nullptr ||
        inf == nullptr) {
        return;
    }
    target_ports_[idx]->attach(inf);
}

void
TmMeshFabric::attach_target(uint32_t idx, p_tm_mem_t mem)
{
    if (idx >= target_ports_.size() || target_ports_[idx] == nullptr ||
        mem == nullptr) {
        return;
    }
    target_ports_[idx]->attach(mem);
}

void
TmMeshFabric::bind_master_id(uint32_t port_id, uint32_t mst_id)
{
    topology_->bind_master_id(port_id, mst_id);
    if (port_id < master_nius_.size() && master_nius_[port_id] != nullptr) {
        master_nius_[port_id]->set_master_id(mst_id);
    }
}

uint32_t
TmMeshFabric::send_rd_req(uint32_t master_port, uint64_t address, uint32_t size)
{
    if (master_port >= master_nius_.size() ||
        master_nius_[master_port] == nullptr) {
        return static_cast<uint32_t>(-1);
    }
    return master_nius_[master_port]->send_rd_req(address, size);
}

uint32_t
TmMeshFabric::send_wr_req(uint32_t master_port, uint64_t address, uint32_t size)
{
    if (master_port >= master_nius_.size() ||
        master_nius_[master_port] == nullptr) {
        return static_cast<uint32_t>(-1);
    }
    return master_nius_[master_port]->send_wr_req(address, size);
}

bool
TmMeshFabric::completed(uint32_t master_port, uint32_t req_id)
{
    if (master_port >= master_nius_.size() ||
        master_nius_[master_port] == nullptr) {
        return false;
    }
    return master_nius_[master_port]->is_request_completed(req_id);
}

bool
TmMeshFabric::canSendRdReq(uint32_t master_port)
{
    if (master_port >= master_nius_.size() ||
        master_nius_[master_port] == nullptr) {
        return false;
    }
    return master_nius_[master_port]->can_send_rd_req();
}

bool
TmMeshFabric::canSendWrReq(uint32_t master_port)
{
    if (master_port >= master_nius_.size() ||
        master_nius_[master_port] == nullptr) {
        return false;
    }
    return master_nius_[master_port]->can_send_wr_req();
}

p_tm_com_que_t
TmMeshFabric::get_mesh_req_fifo(uint32_t router_id, TmMeshPortDir in_dir,
                                aic_req_type_t req_type) const
{
    auto router = routers_[router_id];
    if (req_type == aic_req_type_t::WR_DAT) {
        return router->wr_dat_q(in_dir);
    }
    return router->req_q(in_dir);
}

p_tm_com_que_t
TmMeshFabric::get_mesh_rd_rsp_fifo(uint32_t router_id, TmMeshPortDir in_dir,
                                   uint32_t lane) const
{
    return routers_[router_id]->rd_rsp_q(in_dir, lane);
}

p_tm_com_que_t
TmMeshFabric::get_mesh_wr_req_rsp_fifo(uint32_t router_id,
                                       TmMeshPortDir in_dir) const
{
    return routers_[router_id]->wr_req_rsp_q(in_dir);
}

p_tm_com_que_t
TmMeshFabric::get_mesh_wr_dat_rsp_fifo(uint32_t router_id,
                                       TmMeshPortDir in_dir) const
{
    return routers_[router_id]->wr_dat_rsp_q(in_dir);
}

p_tm_mesh_link_t
TmMeshFabric::get_mesh_link(uint32_t src_router_id, TmMeshPortDir src_dir,
                            uint32_t dst_router_id, TmMeshPortDir dst_dir) const
{
    auto it = links_.find(
        make_link_key(src_router_id, src_dir, dst_router_id, dst_dir));
    return it == links_.end() ? nullptr : it->second;
}

uint64_t
TmMeshFabric::make_link_key(uint32_t src_router_id, TmMeshPortDir src_dir,
                            uint32_t dst_router_id, TmMeshPortDir dst_dir) const
{
    return (static_cast<uint64_t>(src_router_id) << 48) |
           (static_cast<uint64_t>(tm_mesh_port_index(src_dir)) << 40) |
           (static_cast<uint64_t>(dst_router_id) << 8) |
           static_cast<uint64_t>(tm_mesh_port_index(dst_dir));
}

uint64_t
TmMeshFabric::make_txn_key(uint32_t mst_id, uint32_t gid) const
{
    return (static_cast<uint64_t>(mst_id) << 32) | gid;
}

uint64_t
TmMeshFabric::make_txn_key(p_tm_pld_t pld) const
{
    return make_txn_key(pld->mst_id, pld->gid);
}
