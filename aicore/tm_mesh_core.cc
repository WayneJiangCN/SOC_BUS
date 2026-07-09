#include <algorithm>

#include "tm_mesh.h"
#include "tm_mesh_inf.h"
#include "tm_mesh_link.h"
#include "tm_mesh_router.h"
#include "tm_mesh_target_port.h"

using namespace std;
using namespace tm_engine;

TmMeshFabric::TmMeshFabric() {}

TmMeshFabric::TmMeshFabric(p_tm_clk_t clk, p_tm_mesh_cfg_t cfg)
    : clk_(clk), cfg_(cfg) {
  this->name(cfg_->name);
  config();
}

TmMeshFabric::~TmMeshFabric() {}

void TmMeshFabric::config() {
  topology_.config(cfg_);

  auto flow_ctrl_cfg = std::make_shared<tm_bus_cfg_t>();
  flow_ctrl_cfg->num_targets = cfg_->num_targets;
  flow_ctrl_cfg->targets = cfg_->targets;
  flow_ctrl_.config(flow_ctrl_cfg);

  mesh_router_count_ = topology_.router_count();
  mesh_rows_ = topology_.rows();
  mesh_cols_ = topology_.cols();
  mesh_link_latency_ = cfg_->mesh_link_latency;

  master_nius_.clear();
  routers_.clear();
  links_.clear();
  target_ports_.clear();

  next_rd_issue_time_.assign(cfg_->num_targets, 0);
  next_wr_req_issue_time_.assign(cfg_->num_targets, 0);
  next_wr_dat_issue_time_.assign(cfg_->num_targets, 0);

  topology_.reset(cfg_->num_masters);

  for (uint32_t i = 0; i < cfg_->num_masters; ++i) {
    master_nius_.push_back(tm_make_mesh_inf(
        this->name() + "_master_niu" + std::to_string(i), clk_, i, cfg_));
  }

  for (uint32_t i = 0; i < cfg_->num_targets; ++i) {
    auto target_cfg = cfg_->targets[i];
    target_ports_.push_back(tm_make_mesh_target_port(
        this->name() + "_target_port_" + std::to_string(i), clk_, target_cfg,
        cfg_->rd_rsp_port_num, cfg_->target_inf_depth));
  }

  for (uint32_t router = 0; router < mesh_router_count_; ++router) {
    routers_.push_back(tm_make_mesh_router(
        this->name() + "_router_" + std::to_string(router), clk_, cfg_));
  }

  for (uint32_t router = 0; router < mesh_router_count_; ++router) {
    uint32_t row = router / mesh_cols_;
    uint32_t col = router % mesh_cols_;

    if (col + 1 < mesh_cols_) {
      uint32_t east = router + 1;
      links_[make_link_key(router, east)] =
          tm_make_mesh_link(this->name() + "_link_" + std::to_string(router) +
                                "_" + std::to_string(east),
                            mesh_link_latency_);
      links_[make_link_key(east, router)] =
          tm_make_mesh_link(this->name() + "_link_" + std::to_string(east) +
                                "_" + std::to_string(router),
                            mesh_link_latency_);
    }
    if (row + 1 < mesh_rows_) {
      uint32_t south = router + mesh_cols_;
      links_[make_link_key(router, south)] =
          tm_make_mesh_link(this->name() + "_link_" + std::to_string(router) +
                                "_" + std::to_string(south),
                            mesh_link_latency_);
      links_[make_link_key(south, router)] =
          tm_make_mesh_link(this->name() + "_link_" + std::to_string(south) +
                                "_" + std::to_string(router),
                            mesh_link_latency_);
    }
  }

  tm_sensitive(TM_MAKE_CPROC(&TmMeshFabric::tick), clk_->pos_edge);
  reset();
}

void TmMeshFabric::build() {}

void TmMeshFabric::reset() {
  txn_ctx_.clear();

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

  flow_ctrl_.reset();
}

bool TmMeshFabric::idle() {
  bool ret = txn_ctx_.empty();

  for (auto& niu : master_nius_) {
    ret = ret && niu != nullptr && niu->idle();
  }
  for (auto& router : routers_) {
    ret = ret && router != nullptr && router->idle();
  }
  for (auto& target_port : target_ports_) {
    ret = ret && target_port != nullptr && target_port->idle();
  }
  return ret;
}

void TmMeshFabric::tick() {
  for (auto& niu : master_nius_) {
    if (niu != nullptr) {
      niu->tick();
    }
  }
  flow_ctrl_.update_tokens(time());
  recv_target_rsps();
  recv_master_reqs();
  inject_mesh_reqs();
  advance_mesh_routers();
  send_target_reqs();
}

void TmMeshFabric::attach_master(uint32_t idx, p_tm_mesh_inf_t inf) {
  if (idx >= master_nius_.size() || inf == nullptr) {
    return;
  }

  inf->set_master_id(topology_.port_master_id(idx));
  master_nius_[idx] = inf;
}

void TmMeshFabric::attach_master(p_tm_mesh_inf_t inf) {
  if (inf == nullptr) {
    return;
  }
  attach_master(inf->inf_id_, inf);
}

void TmMeshFabric::attach_master(uint32_t idx, p_tm_com_inf_t inf) {
  if (idx >= master_nius_.size() || master_nius_[idx] == nullptr ||
      inf == nullptr) {
    return;
  }
  master_nius_[idx]->attach_upstream(inf);
}
//主要接口
void TmMeshFabric::attach_master(uint32_t idx, p_pem_biu_t biu) {
  if (biu == nullptr) {
    return;
  }
  attach_master(idx, biu->out_intf_);
  bind_master_id(idx, biu->core_id_);
}

void TmMeshFabric::attach_master(p_pem_biu_t biu) {
  if (biu == nullptr) {
    return;
  }
  attach_master(biu->core_id_, biu);
}

void TmMeshFabric::attach_target(uint32_t idx, p_tm_com_inf_t inf) {
  if (idx >= target_ports_.size() || target_ports_[idx] == nullptr ||
      inf == nullptr) {
    return;
  }
  target_ports_[idx]->attach_downstream(inf);
}
//主要接口
void TmMeshFabric::attach_target(uint32_t idx, p_tm_mem_t mem) {
  if (idx >= target_ports_.size() || target_ports_[idx] == nullptr ||
      mem == nullptr) {
    return;
  }
  target_ports_[idx]->attach_downstream(mem);
}

void TmMeshFabric::bind_master_id(uint32_t port_id, uint32_t mst_id) {
  topology_.bind_master_id(port_id, mst_id);
  if (port_id < master_nius_.size() && master_nius_[port_id] != nullptr) {
    master_nius_[port_id]->set_master_id(mst_id);
  }
}

uint32_t TmMeshFabric::send_rd_req(uint32_t master_port, uint64_t address,
                                   uint32_t size) {
  if (master_port >= master_nius_.size() ||
      master_nius_[master_port] == nullptr) {
    return static_cast<uint32_t>(-1);
  }
  return master_nius_[master_port]->send_rd_req(address, size);
}

uint32_t TmMeshFabric::send_wr_req(uint32_t master_port, uint64_t address,
                                   uint32_t size) {
  if (master_port >= master_nius_.size() ||
      master_nius_[master_port] == nullptr) {
    return static_cast<uint32_t>(-1);
  }
  return master_nius_[master_port]->send_wr_req(address, size);
}

bool TmMeshFabric::completed(uint32_t master_port, uint32_t req_id) {
  if (master_port >= master_nius_.size() ||
      master_nius_[master_port] == nullptr) {
    return false;
  }
  return master_nius_[master_port]->is_request_completed(req_id);
}

bool TmMeshFabric::canSendRdReq(uint32_t master_port) {
  if (master_port >= master_nius_.size() ||
      master_nius_[master_port] == nullptr) {
    return false;
  }
  return master_nius_[master_port]->can_send_rd_req();
}

bool TmMeshFabric::canSendWrReq(uint32_t master_port) {
  if (master_port >= master_nius_.size() ||
      master_nius_[master_port] == nullptr) {
    return false;
  }
  return master_nius_[master_port]->can_send_wr_req();
}

p_tm_com_que_t TmMeshFabric::get_target_fifo(uint32_t target_id,
                                             aic_req_type_t req_type) const {
  return target_ports_[target_id]->req_q(req_type);
}

p_tm_com_que_t TmMeshFabric::get_mesh_req_fifo(uint32_t router_id,
                                               aic_req_type_t req_type) const {
  auto router = routers_[router_id];
  if (req_type == aic_req_type_t::WR_DAT) {
    return router->wr_dat_q();
  }
  return router->req_q();
}

p_tm_com_que_t TmMeshFabric::get_mesh_rd_rsp_fifo(uint32_t router_id,
                                                  uint32_t lane) const {
  return routers_[router_id]->rd_rsp_q(lane);
}

p_tm_com_que_t TmMeshFabric::get_mesh_wr_req_rsp_fifo(
    uint32_t router_id) const {
  return routers_[router_id]->wr_req_rsp_q();
}

p_tm_com_que_t TmMeshFabric::get_mesh_wr_dat_rsp_fifo(
    uint32_t router_id) const {
  return routers_[router_id]->wr_dat_rsp_q();
}

p_tm_mesh_link_t TmMeshFabric::get_mesh_link(uint32_t src_router_id,
                                             uint32_t dst_router_id) const {
  auto it = links_.find(make_link_key(src_router_id, dst_router_id));
  return it == links_.end() ? nullptr : it->second;
}

uint64_t TmMeshFabric::make_link_key(uint32_t src_router_id,
                                     uint32_t dst_router_id) const {
  return (static_cast<uint64_t>(src_router_id) << 32) | dst_router_id;
}

uint64_t TmMeshFabric::make_txn_key(uint32_t mst_id, uint32_t gid) const {
  return (static_cast<uint64_t>(mst_id) << 32) | gid;
}

uint64_t TmMeshFabric::make_txn_key(p_tm_pld_t pld) const {
  return make_txn_key(pld->mst_id, pld->gid);
}
