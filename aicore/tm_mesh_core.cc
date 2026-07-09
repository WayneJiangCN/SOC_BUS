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
  // 先根据用户配置初始化拓扑和 target 侧 flow-control。
  topology_.config(cfg_);

  auto flow_ctrl_cfg = std::make_shared<tm_bus_cfg_t>();
  flow_ctrl_cfg->num_targets = cfg_->num_targets;
  flow_ctrl_cfg->targets = cfg_->targets;
  flow_ctrl_.config(flow_ctrl_cfg);

  mesh_router_count_ = topology_.router_count();
  mesh_rows_ = topology_.rows();
  mesh_cols_ = topology_.cols();
  mesh_link_latency_ = cfg_->mesh_link_latency;

  // 重新创建整张网络前，先清空旧对象。
  master_nius_.clear();
  routers_.clear();
  links_.clear();
  target_ports_.clear();

  next_rd_issue_time_.assign(cfg_->num_targets, 0);
  next_wr_req_issue_time_.assign(cfg_->num_targets, 0);
  next_wr_dat_issue_time_.assign(cfg_->num_targets, 0);

  topology_.reset(cfg_->num_masters);

  // 为每个 master 创建一个本地 NIU。
  for (uint32_t i = 0; i < cfg_->num_masters; ++i) {
    master_nius_.push_back(tm_make_mesh_inf(
        this->name() + "_master_niu" + std::to_string(i), clk_, i, cfg_));
  }

  // 为每个 target 创建一个 target-side endpoint。
  for (uint32_t i = 0; i < cfg_->num_targets; ++i) {
    auto target_cfg = cfg_->targets[i];
    target_ports_.push_back(tm_make_mesh_target_port(
        this->name() + "_target_port_" + std::to_string(i), clk_, target_cfg,
        cfg_->rd_rsp_port_num, cfg_->target_inf_depth));
  }

  // 创建所有 router 节点。
  for (uint32_t router = 0; router < mesh_router_count_; ++router) {
    routers_.push_back(tm_make_mesh_router(
        this->name() + "_router_" + std::to_string(router), clk_, cfg_));
  }

  // 按 topology 构建明确的 port-to-port 有向链路。
  for (uint32_t router = 0; router < mesh_router_count_; ++router) {
    if (topology_.has_neighbor(router, TmMeshPortDir::EAST)) {
      uint32_t east = topology_.neighbor(router, TmMeshPortDir::EAST);
      links_[make_link_key(router, TmMeshPortDir::EAST, east,
                           TmMeshPortDir::WEST)] =
          tm_make_mesh_link(this->name() + "_link_" + std::to_string(router) +
                                "_E_" + std::to_string(east) + "_W",
                            mesh_link_latency_, router, TmMeshPortDir::EAST,
                            east, TmMeshPortDir::WEST);
    }
    if (topology_.has_neighbor(router, TmMeshPortDir::WEST)) {
      uint32_t west = topology_.neighbor(router, TmMeshPortDir::WEST);
      links_[make_link_key(router, TmMeshPortDir::WEST, west,
                           TmMeshPortDir::EAST)] =
          tm_make_mesh_link(this->name() + "_link_" + std::to_string(router) +
                                "_W_" + std::to_string(west) + "_E",
                            mesh_link_latency_, router, TmMeshPortDir::WEST,
                            west, TmMeshPortDir::EAST);
    }
    if (topology_.has_neighbor(router, TmMeshPortDir::SOUTH)) {
      uint32_t south = topology_.neighbor(router, TmMeshPortDir::SOUTH);
      links_[make_link_key(router, TmMeshPortDir::SOUTH, south,
                           TmMeshPortDir::NORTH)] =
          tm_make_mesh_link(this->name() + "_link_" + std::to_string(router) +
                                "_S_" + std::to_string(south) + "_N",
                            mesh_link_latency_, router, TmMeshPortDir::SOUTH,
                            south, TmMeshPortDir::NORTH);
    }
    if (topology_.has_neighbor(router, TmMeshPortDir::NORTH)) {
      uint32_t north = topology_.neighbor(router, TmMeshPortDir::NORTH);
      links_[make_link_key(router, TmMeshPortDir::NORTH, north,
                           TmMeshPortDir::SOUTH)] =
          tm_make_mesh_link(this->name() + "_link_" + std::to_string(router) +
                                "_N_" + std::to_string(north) + "_S",
                            mesh_link_latency_, router, TmMeshPortDir::NORTH,
                            north, TmMeshPortDir::SOUTH);
    }
  }

  tm_sensitive(TM_MAKE_CPROC(&TmMeshFabric::tick), clk_->pos_edge);
  reset();
}

void TmMeshFabric::build() {}

void TmMeshFabric::reset() {
  // 共享事务表先清空，后续各子模块各自 reset。
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
  for (const auto& it : links_) {
    ret = ret && (it.second == nullptr || it.second->idle());
  }
  return ret;
}

void TmMeshFabric::tick() {
  // 整张网络每拍都按固定顺序推进，避免请求/响应交错导致语义混乱。
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

  // NIU 接到 fabric 后，master_id 由 topology 的绑定关系决定。
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

/* 直接挂接一个下游 target 接口。 */
void TmMeshFabric::attach_target(uint32_t idx, p_tm_com_inf_t inf) {
  if (idx >= target_ports_.size() || target_ports_[idx] == nullptr ||
      inf == nullptr) {
    return;
  }
  target_ports_[idx]->attach_downstream(inf);
}

/* 直接挂接一个 TmMem，对外仍通过 TargetPort 的 inf() 握手。 */
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
  // TargetPort 内部仍按请求类型分开本地缓存。
  return target_ports_[target_id]->req_q(req_type);
}

p_tm_com_que_t TmMeshFabric::get_mesh_req_fifo(uint32_t router_id,
                                               TmMeshPortDir in_dir,
                                               aic_req_type_t req_type) const {
  auto router = routers_[router_id];
  if (req_type == aic_req_type_t::WR_DAT) {
    return router->wr_dat_q(in_dir);
  }
  return router->req_q(in_dir);
}

p_tm_com_que_t TmMeshFabric::get_mesh_rd_rsp_fifo(uint32_t router_id,
                                                  TmMeshPortDir in_dir,
                                                  uint32_t lane) const {
  return routers_[router_id]->rd_rsp_q(in_dir, lane);
}

p_tm_com_que_t TmMeshFabric::get_mesh_wr_req_rsp_fifo(
    uint32_t router_id, TmMeshPortDir in_dir) const {
  return routers_[router_id]->wr_req_rsp_q(in_dir);
}

p_tm_com_que_t TmMeshFabric::get_mesh_wr_dat_rsp_fifo(
    uint32_t router_id, TmMeshPortDir in_dir) const {
  return routers_[router_id]->wr_dat_rsp_q(in_dir);
}

p_tm_mesh_link_t TmMeshFabric::get_mesh_link(uint32_t src_router_id,
                                             TmMeshPortDir src_dir,
                                             uint32_t dst_router_id,
                                             TmMeshPortDir dst_dir) const {
  // 当前 link 是明确的 src_port -> dst_port 有向边。
  auto it = links_.find(
      make_link_key(src_router_id, src_dir, dst_router_id, dst_dir));
  return it == links_.end() ? nullptr : it->second;
}

uint64_t TmMeshFabric::make_link_key(uint32_t src_router_id,
                                     TmMeshPortDir src_dir,
                                     uint32_t dst_router_id,
                                     TmMeshPortDir dst_dir) const {
  return (static_cast<uint64_t>(src_router_id) << 48) |
         (static_cast<uint64_t>(tm_mesh_port_index(src_dir)) << 40) |
         (static_cast<uint64_t>(dst_router_id) << 8) |
         static_cast<uint64_t>(tm_mesh_port_index(dst_dir));
}

uint64_t TmMeshFabric::make_txn_key(uint32_t mst_id, uint32_t gid) const {
  return (static_cast<uint64_t>(mst_id) << 32) | gid;
}

uint64_t TmMeshFabric::make_txn_key(p_tm_pld_t pld) const {
  return make_txn_key(pld->mst_id, pld->gid);
}
