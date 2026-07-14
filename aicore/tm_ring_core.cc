#include <algorithm>

#include "pem_biu.h"
#include "tm_ring.h"
#include "tm_ring_inf.h"
#include "tm_ring_link.h"
#include "tm_ring_router.h"
#include "tm_ring_target_port.h"
#include "tm_pld.h"

using namespace std;
using namespace tm_engine;

TmRingFabric::TmRingFabric()
{
}

TmRingFabric::TmRingFabric(p_tm_clk_t clk, p_tm_ring_cfg_t cfg)
    : clk_(clk), cfg_(cfg)
{
    this->name(cfg_->name);
    config();
}

TmRingFabric::~TmRingFabric()
{
}

void
TmRingFabric::config()
{
    topology_ = std::make_shared<TmRingTopology>();
    flow_ctrl_ = std::make_shared<TmBusFlowCtrl>();

    topology_->config(cfg_);

    auto flow_ctrl_cfg = std::make_shared<tm_bus_cfg_t>();
    flow_ctrl_cfg->num_targets = cfg_->num_targets;
    flow_ctrl_cfg->targets = cfg_->targets;
    flow_ctrl_->config(flow_ctrl_cfg);

    ring_router_count_ = topology_->router_count();
    ring_link_latency_ = cfg_->ring_link_latency;

    master_nius_.clear();
    routers_.clear();
    links_.clear();
    target_ports_.clear();

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
        master_nius_.push_back(tm_make_ring_inf(
            this->name() + "_master_niu" + std::to_string(i), clk_, i, cfg_));
    }

    for (uint32_t i = 0; i < cfg_->num_targets; ++i) {
        auto target_cfg = cfg_->targets[i];
        target_ports_.push_back(tm_make_ring_target_port(
            this->name() + "_target_port_" + std::to_string(i), clk_,
            target_cfg, cfg_->rd_rsp_port_num, cfg_->target_inf_depth));
    }

    for (uint32_t router = 0; router < ring_router_count_; ++router) {
        routers_.push_back(tm_make_ring_router(
            this->name() + "_router_" + std::to_string(router), clk_, cfg_));
    }

    for (uint32_t router = 0; router < ring_router_count_; ++router) {
        if (topology_->has_neighbor(router, TmRingPortDir::EAST)) {
            uint32_t east = topology_->neighbor(router, TmRingPortDir::EAST);
            links_[make_link_key(router, TmRingPortDir::EAST, east,
                                 TmRingPortDir::WEST)] =
                tm_make_ring_link(this->name() + "_link_" +
                                      std::to_string(router) + "_E_" +
                                      std::to_string(east) + "_W",
                                  clk_, cfg_, ring_link_latency_, east,
                                  TmRingPortDir::WEST);
        }
        if (topology_->has_neighbor(router, TmRingPortDir::WEST)) {
            uint32_t west = topology_->neighbor(router, TmRingPortDir::WEST);
            links_[make_link_key(router, TmRingPortDir::WEST, west,
                                 TmRingPortDir::EAST)] =
                tm_make_ring_link(this->name() + "_link_" +
                                      std::to_string(router) + "_W_" +
                                      std::to_string(west) + "_E",
                                  clk_, cfg_, ring_link_latency_, west,
                                  TmRingPortDir::EAST);
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
                    get_router_req_fifo(source_router, TmRingPortDir::LOCAL,
                                        aic_req_type_t::RD_REQ),
                    get_router_req_fifo(source_router, TmRingPortDir::LOCAL,
                                        aic_req_type_t::WR_DAT),
                    [this](aic_req_type_t req_type) {
                        return reserve_global_osd(req_type);
                    });
    }

    // Routers own route calculation, link backpressure and link injection.
    // Fabric remains the local endpoint for target/NIU protocol handling.
    for (uint32_t router_id = 0; router_id < routers_.size(); ++router_id) {
        auto router = routers_[router_id];
        if (router == nullptr) {
            continue;
        }
        auto east_link = topology_->has_neighbor(router_id, TmRingPortDir::EAST)
                             ? get_ring_link(
                                   router_id, TmRingPortDir::EAST,
                                   topology_->neighbor(router_id,
                                                       TmRingPortDir::EAST),
                                   TmRingPortDir::WEST)
                             : nullptr;
        auto west_link = topology_->has_neighbor(router_id, TmRingPortDir::WEST)
                             ? get_ring_link(
                                   router_id, TmRingPortDir::WEST,
                                   topology_->neighbor(router_id,
                                                       TmRingPortDir::WEST),
                                   TmRingPortDir::EAST)
                             : nullptr;
        router->attach(router_id, topology_, east_link, west_link, this);
    }

    // Link queues own their local vld binding. Fabric only provides the target
    // FIFO lookup.
    for (const auto& it : links_) {
        auto link = it.second;
        if (link != nullptr) {
            link->attach(
                [this](uint32_t dst_router, TmRingPortDir dst_dir,
                       uint32_t traffic_class, p_tm_pld_t pld) {
                    auto cmd = static_cast<PldCmd>(traffic_class);
                    if (cmd == PldCmd::RD || cmd == PldCmd::WR) {
                        return get_router_req_fifo(dst_router, dst_dir,
                                                   static_cast<aic_req_type_t>(
                                                       tm_pld_req_type(pld)));
                    }
                    if (cmd == PldCmd::WR_DAT) {
                        return get_router_req_fifo(dst_router, dst_dir,
                                                   aic_req_type_t::WR_DAT);
                    }
                    if (cmd == PldCmd::WR_RSP) {
                        return get_router_wr_req_rsp_fifo(dst_router, dst_dir);
                    }
                    if (cmd == PldCmd::RSP) {
                        return get_router_wr_dat_rsp_fifo(dst_router, dst_dir);
                    }

                    return get_router_rd_rsp_fifo(dst_router, dst_dir,
                                                  pld->ring_rsp_lane);
                });
        }
    }

    // TargetPort owns target-side queue, memory handshake and response events.
    for (uint32_t target_id = 0; target_id < target_ports_.size();
         ++target_id) {
        auto target_port = target_ports_[target_id];
        if (target_port == nullptr) {
            continue;
        }

        uint32_t router_id = topology_->target_node(target_id);
        std::vector<p_tm_com_que_t> rd_rsp_qs;
        for (uint32_t lane = 0; lane < cfg_->rd_rsp_port_num; ++lane) {
            rd_rsp_qs.push_back(get_router_rd_rsp_fifo(
                router_id, TmRingPortDir::LOCAL, lane));
        }
        target_port->attach(
            target_id, flow_ctrl_, rd_rsp_qs,
            get_router_wr_req_rsp_fifo(router_id, TmRingPortDir::LOCAL),
            get_router_wr_dat_rsp_fifo(router_id, TmRingPortDir::LOCAL));
    }

    if (token_clk_ != nullptr) {
        tm_sensitive(TM_MAKE_CPROC(&TmRingFabric::update_target_tokens),
                     token_clk_->pos_edge);
    }

    reset();
}

void
TmRingFabric::build()
{
}

void
TmRingFabric::reset()
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

    global_outstanding_ = 0;

    flow_ctrl_->reset();
}

bool
TmRingFabric::idle()
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
    ret = ret && global_outstanding_ == 0;
    return ret;
}

void
TmRingFabric::update_target_tokens()
{
    flow_ctrl_->update_tokens(time());
    for (auto& target_port : target_ports_) {
        if (target_port != nullptr) {
            target_port->send_pending_requests();
        }
    }
}

void
TmRingFabric::attach_master(uint32_t idx, p_tm_ring_inf_t inf)
{
    if (idx >= master_nius_.size() || inf == nullptr) {
        return;
    }

    inf->set_master_id(topology_->port_master_id(idx));
    master_nius_[idx] = inf;
    uint32_t source_router = topology_->master_node(idx);
    inf->attach(idx, topology_, flow_ctrl_,
                get_router_req_fifo(source_router, TmRingPortDir::LOCAL,
                                    aic_req_type_t::RD_REQ),
                get_router_req_fifo(source_router, TmRingPortDir::LOCAL,
                                    aic_req_type_t::WR_DAT),
                [this](aic_req_type_t req_type) {
                    return reserve_global_osd(req_type);
                });
}

void
TmRingFabric::attach_master(p_tm_ring_inf_t inf)
{
    if (inf == nullptr) {
        return;
    }
    attach_master(inf->inf_id_, inf);
}

void
TmRingFabric::attach_master(uint32_t idx, p_tm_ring_biu_t biu)
{
    if (biu == nullptr) {
        return;
    }
    attach_master(idx, biu->out_intf_);
}

void
TmRingFabric::attach_master(uint32_t idx, p_tm_com_inf_t inf)
{
    if (idx >= master_nius_.size() || master_nius_[idx] == nullptr ||
        inf == nullptr) {
        return;
    }
    master_nius_[idx]->attach(inf);
}

void
TmRingFabric::attach_target(uint32_t idx, p_tm_com_inf_t inf)
{
    if (idx >= target_ports_.size() || target_ports_[idx] == nullptr ||
        inf == nullptr) {
        return;
    }
    target_ports_[idx]->attach(inf);
}

void
TmRingFabric::attach_target(uint32_t idx, p_tm_mem_t mem)
{
    if (idx >= target_ports_.size() || target_ports_[idx] == nullptr ||
        mem == nullptr) {
        return;
    }
    target_ports_[idx]->attach(mem);
}

void
TmRingFabric::bind_master_id(uint32_t port_id, uint32_t mst_id)
{
    topology_->bind_master_id(port_id, mst_id);
    if (port_id < master_nius_.size() && master_nius_[port_id] != nullptr) {
        master_nius_[port_id]->set_master_id(mst_id);
    }
}

uint32_t
TmRingFabric::send_rd_req(uint32_t master_port, uint64_t address, uint32_t size)
{
    if (master_port >= master_nius_.size() ||
        master_nius_[master_port] == nullptr) {
        return static_cast<uint32_t>(-1);
    }
    return master_nius_[master_port]->send_rd_req(address, size);
}

uint32_t
TmRingFabric::send_wr_req(uint32_t master_port, uint64_t address, uint32_t size)
{
    if (master_port >= master_nius_.size() ||
        master_nius_[master_port] == nullptr) {
        return static_cast<uint32_t>(-1);
    }
    return master_nius_[master_port]->send_wr_req(address, size);
}

bool
TmRingFabric::completed(uint32_t master_port, uint32_t req_id)
{
    if (master_port >= master_nius_.size() ||
        master_nius_[master_port] == nullptr) {
        return false;
    }
    return master_nius_[master_port]->is_request_completed(req_id);
}

bool
TmRingFabric::canSendRdReq(uint32_t master_port)
{
    if (master_port >= master_nius_.size() ||
        master_nius_[master_port] == nullptr) {
        return false;
    }
    return master_nius_[master_port]->can_send_rd_req();
}

bool
TmRingFabric::canSendWrReq(uint32_t master_port)
{
    if (master_port >= master_nius_.size() ||
        master_nius_[master_port] == nullptr) {
        return false;
    }
    return master_nius_[master_port]->can_send_wr_req();
}

p_tm_com_que_t
TmRingFabric::get_router_req_fifo(uint32_t router_id, TmRingPortDir in_dir,
                                  aic_req_type_t req_type) const
{
    auto router = routers_[router_id];
    if (req_type == aic_req_type_t::WR_DAT) {
        return router->wr_dat_q(in_dir);
    }
    return router->req_q(in_dir);
}

p_tm_com_que_t
TmRingFabric::get_router_rd_rsp_fifo(uint32_t router_id, TmRingPortDir in_dir,
                                     uint32_t lane) const
{
    return routers_[router_id]->rd_rsp_q(in_dir, lane);
}

p_tm_com_que_t
TmRingFabric::get_router_wr_req_rsp_fifo(uint32_t router_id,
                                         TmRingPortDir in_dir) const
{
    return routers_[router_id]->wr_req_rsp_q(in_dir);
}

p_tm_com_que_t
TmRingFabric::get_router_wr_dat_rsp_fifo(uint32_t router_id,
                                         TmRingPortDir in_dir) const
{
    return routers_[router_id]->wr_dat_rsp_q(in_dir);
}

p_tm_ring_link_t
TmRingFabric::get_ring_link(uint32_t src_router_id, TmRingPortDir src_dir,
                            uint32_t dst_router_id, TmRingPortDir dst_dir) const
{
    auto it = links_.find(
        make_link_key(src_router_id, src_dir, dst_router_id, dst_dir));
    return it == links_.end() ? nullptr : it->second;
}

uint64_t
TmRingFabric::make_link_key(uint32_t src_router_id, TmRingPortDir src_dir,
                            uint32_t dst_router_id, TmRingPortDir dst_dir) const
{
    return (static_cast<uint64_t>(src_router_id) << 48) |
           (static_cast<uint64_t>(tm_ring_port_index(src_dir)) << 40) |
           (static_cast<uint64_t>(dst_router_id) << 8) |
           static_cast<uint64_t>(tm_ring_port_index(dst_dir));
}

uint64_t
TmRingFabric::make_txn_key(uint32_t mst_id, uint32_t gid) const
{
    return (static_cast<uint64_t>(mst_id) << 32) | gid;
}

uint64_t
TmRingFabric::make_txn_key(p_tm_pld_t pld) const
{
    return make_txn_key(pld->mst_id, pld->gid);
}
