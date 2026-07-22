#include "pem_biu.h"
#include "tm_ring.h"
#include "tm_ring_inf.h"
#include "tm_ring_link.h"
#include "tm_ring_router.h"
#include "tm_ring_target_port.h"

using namespace std;
using namespace tm_engine;

namespace {

const char* ring_dir_name(TmRingPortDir dir) {
  if (dir == TmRingPortDir::EAST) {
    return "E";
  }
  if (dir == TmRingPortDir::WEST) {
    return "W";
  }
  return "L";
}

}  // namespace

TmRingFabric::TmRingFabric() {}

TmRingFabric::TmRingFabric(p_tm_clk_t clk, p_tm_ring_cfg_t cfg)
    : clk_(clk), cfg_(cfg) {
  this->name(cfg_->name);
  config();
}

TmRingFabric::~TmRingFabric() {}

void TmRingFabric::config() {
  log_para_t log_para(this->name() + ".log");
  log_ = pem_log::create_logger(log_para);
  PEM_LOG_INFO(log_, "[{0:d}] config masters:{1:d} targets:{2:d}",
               time(), cfg_->num_masters, cfg_->num_targets);

  // 构建顺序必须先于连接顺序：连接阶段会按拓扑索引访问已创建的子模块。
  init_topology_and_flow_ctrl();
  clear_components();

  create_master_nius();
  create_target_ports();
  create_routers();
  create_links();

  bind_master_nius();
  attach_routers();
  attach_links();
  bind_target_ports();

  reset();
}

void TmRingFabric::init_topology_and_flow_ctrl() {
  // Topology 负责“包去哪里”，FlowCtrl 负责“Target 当前能否接收”。
  topology_ = std::make_shared<TmRingTopology>();
  flow_ctrl_ = std::make_shared<TmBusFlowCtrl>();

  topology_->config(cfg_);

  // Ring 配置与总线流控配置共享 Target 参数，避免维护第二套 Target 资源状态。
  auto flow_ctrl_cfg = std::make_shared<tm_bus_cfg_t>();
  flow_ctrl_cfg->num_targets = cfg_->num_targets;
  flow_ctrl_cfg->global_osd = cfg_->global_osd;
  flow_ctrl_cfg->targets = cfg_->targets;
  flow_ctrl_->config(flow_ctrl_cfg);

  ring_router_count_ = topology_->router_count();
  ring_link_latency_ = cfg_->ring_link_latency;

  topology_->reset(cfg_->num_masters);
}

void TmRingFabric::clear_components() {
  master_nius_.clear();
  routers_.clear();
  links_.clear();
  target_ports_.clear();
}

void TmRingFabric::create_master_nius() {
  // 每个 Master 独占一个 NIU，API 请求号和 Master OSD 因此互不干扰。
  for (uint32_t i = 0; i < cfg_->num_masters; ++i) {
    master_nius_.push_back(tm_make_ring_inf(
        this->name() + "_master_niu" + std::to_string(i), clk_, i, cfg_));
  }
}

void TmRingFabric::create_target_ports() {
  for (uint32_t i = 0; i < cfg_->num_targets; ++i) {
    auto target_cfg = cfg_->targets[i];
    target_ports_.push_back(tm_make_ring_target_port(
        this->name() + "_target_port_" + std::to_string(i), clk_, target_cfg,
        cfg_->rd_rsp_port_num));
  }
}

void TmRingFabric::create_routers() {
  for (uint32_t router = 0; router < ring_router_count_; ++router) {
    routers_.push_back(tm_make_ring_router(
        this->name() + "_router_" + std::to_string(router), clk_, cfg_));
  }
}

void TmRingFabric::create_links() {
  // 每个 Router 分别建立顺时针和逆时针输出 Link，形成双向 Ring。
  for (uint32_t router = 0; router < ring_router_count_; ++router) {
    create_link(router, TmRingPortDir::EAST);
    create_link(router, TmRingPortDir::WEST);
  }
}

void TmRingFabric::create_link(uint32_t router_id, TmRingPortDir out_dir) {
  // 单节点 Ring 没有外部邻居，此时不创建无意义的自环 Link。
  if (!topology_->has_neighbor(router_id, out_dir)) {
    return;
  }

  uint32_t dst_router = topology_->neighbor(router_id, out_dir);
  auto dst_dir = tm_ring_opposite_dir(out_dir);
  auto link_name = this->name() + "_link_" + std::to_string(router_id) + "_" +
                   ring_dir_name(out_dir) + "_" + std::to_string(dst_router) +
                   "_" + ring_dir_name(dst_dir);

  links_[make_link_key(router_id, out_dir, dst_router, dst_dir)] =
      tm_make_ring_link(link_name, clk_, cfg_, ring_link_latency_, dst_router,
                        dst_dir);
  PEM_LOG_INFO(log_, "[{0:d}] create_link src_router:{1:d} dir:{2:d} "
                     "dst_router:{3:d} dst_dir:{4:d}",
               time(), router_id, tm_ring_port_index(out_dir), dst_router,
               tm_ring_port_index(dst_dir));
}

void TmRingFabric::bind_master_nius() {
  for (uint32_t i = 0; i < master_nius_.size(); ++i) {
    bind_master_niu(i, master_nius_[i]);
  }
}

void TmRingFabric::bind_master_niu(uint32_t idx, p_tm_ring_inf_t inf) {
  // 先写入稳定 mst_id，再把 NIU 的 Router 侧端口挂到源节点 LOCAL 口。
  inf->set_master_id(topology_->port_master_id(idx));
  master_nius_[idx] = inf;

  uint32_t source_router = topology_->master_node(idx);
  inf->attach(idx, topology_, flow_ctrl_);
  routers_[source_router]->bind_local_master(idx, inf->router_inf());
  PEM_LOG_INFO(log_, "[{0:d}] bind_master port:{1:d} mst_id:{2:d} "
                     "router:{3:d}",
               time(), idx, topology_->port_master_id(idx), source_router);
}

void TmRingFabric::attach_routers() {
  // Router 只持有自己的两个输出 Link，不直接访问全局 Link 容器。
  for (uint32_t router_id = 0; router_id < routers_.size(); ++router_id) {
    auto router = routers_[router_id];
    auto east_link =
        topology_->has_neighbor(router_id, TmRingPortDir::EAST)
            ? get_ring_link(router_id, TmRingPortDir::EAST,
                            topology_->neighbor(router_id, TmRingPortDir::EAST),
                            TmRingPortDir::WEST)
            : nullptr;
    auto west_link =
        topology_->has_neighbor(router_id, TmRingPortDir::WEST)
            ? get_ring_link(router_id, TmRingPortDir::WEST,
                            topology_->neighbor(router_id, TmRingPortDir::WEST),
                            TmRingPortDir::EAST)
            : nullptr;
    router->attach(router_id, topology_, east_link, west_link);
  }
}

void TmRingFabric::attach_links() {
  // Link 的目的方向是相邻 Router 的输入方向，例如 EAST 输出接对端 WEST 输入。
  for (const auto& it : links_) {
    auto link = it.second;
    link->attach(get_router_port_inf(link->dst_router(), link->dst_dir()));
  }
}

void TmRingFabric::bind_target_ports() {
  // TargetPort 挂在 topology 分配的节点 LOCAL 口，负责 Ring 与 Memory 的协议转换。
  for (uint32_t target_id = 0; target_id < target_ports_.size(); ++target_id) {
    auto target_port = target_ports_[target_id];
    uint32_t router_id = topology_->target_node(target_id);
    target_port->attach(target_id, flow_ctrl_);
    routers_[router_id]->bind_local_target(target_id, target_port->ring_inf());
    PEM_LOG_INFO(log_, "[{0:d}] bind_target target:{1:d} router:{2:d}",
                 time(), target_id, router_id);
  }
}

void TmRingFabric::build() {}

void TmRingFabric::reset() {
  PEM_LOG_INFO(log_, "[{0:d}] reset", time());
  // 先清空所有承载 payload 的子模块，再恢复共享 credit/token/outstanding。
  for (auto& niu : master_nius_) {
    niu->reset();
  }
  for (auto& router : routers_) {
    router->reset();
  }
  for (auto& it : links_) {
    it.second->reset();
  }
  for (auto& target_port : target_ports_) {
    target_port->reset();
  }

  flow_ctrl_->reset();
}

bool TmRingFabric::idle() {
  // 任一层仍有在途包或未完成事务时，整个 Fabric 都不能报告空闲。
  bool ret = true;

  for (auto& niu : master_nius_) {
    ret = ret && niu->idle();
  }
  for (auto& router : routers_) {
    ret = ret && router->idle();
  }
  for (auto& target_port : target_ports_) {
    ret = ret && target_port->idle();
  }
  for (const auto& it : links_) {
    ret = ret && it.second->idle();
  }
  return ret;
}

uint64_t TmRingFabric::global_osd_stalls() const {
  return flow_ctrl_ == nullptr ? 0 : flow_ctrl_->global_osd_stalls();
}

uint64_t TmRingFabric::target_slot_stalls() const {
  return flow_ctrl_ == nullptr ? 0 : flow_ctrl_->target_slot_stalls();
}

uint64_t TmRingFabric::bandwidth_token_stalls() const {
  return flow_ctrl_ == nullptr ? 0 : flow_ctrl_->bandwidth_token_stalls();
}

TmRingLinkStallBreakdown TmRingFabric::ring_link_stall_breakdown() const {
  TmRingLinkStallBreakdown stalls;
  for (const auto& it : links_) {
    const auto& req = it.second->subnet_stats(TmRingSubnet::REQ);
    const auto& rsp = it.second->subnet_stats(TmRingSubnet::RSP);
    stalls.serialization_busy += req.serialization_busy_stall +
                                 rsp.serialization_busy_stall;
    stalls.inflight_limit += req.inflight_limit_stall +
                             rsp.inflight_limit_stall;
    stalls.link_fifo_full += req.link_fifo_full_stall +
                             rsp.link_fifo_full_stall;
    stalls.bubble_reserved += req.bubble_reserved_stall +
                               rsp.bubble_reserved_stall;
    stalls.downstream_fifo_full += req.downstream_inf_full_stall +
                                   rsp.downstream_inf_full_stall;
  }
  return stalls;
}

uint64_t TmRingFabric::ring_link_stalls() const {
  return ring_link_stall_breakdown().total();
}

void TmRingFabric::attach_master(uint32_t idx, p_tm_ring_inf_t inf) {
  // 越界或空接口无法形成有效连接，直接拒绝且不覆盖已有 NIU。
  if (idx >= master_nius_.size() || inf == nullptr) {
    return;
  }

  bind_master_niu(idx, inf);
  PEM_LOG_INFO(log_, "[{0:d}] attach_master_niu port:{1:d}", time(), idx);
}

void TmRingFabric::attach_master(p_tm_ring_inf_t inf) {
  if (inf == nullptr) {
    return;
  }
  attach_master(inf->inf_id_, inf);
}

void TmRingFabric::attach_master(uint32_t idx, p_tm_ring_biu_t biu) {
  if (biu == nullptr) {
    return;
  }
  // BIU 的 out_intf_ 同时承载下行请求和上行响应，可直接接入 NIU bus_inf_。
  attach_master(idx, biu->out_intf_);
}

void TmRingFabric::attach_master(uint32_t idx, p_tm_com_inf_t inf) {
  if (idx >= master_nius_.size() || inf == nullptr) {
    return;
  }
  master_nius_[idx]->attach(inf);
  PEM_LOG_INFO(log_, "[{0:d}] attach_master_inf port:{1:d}", time(), idx);
}

void TmRingFabric::attach_target(uint32_t idx, p_tm_com_inf_t inf) {
  if (idx >= target_ports_.size() || inf == nullptr) {
    return;
  }
  target_ports_[idx]->attach(inf);
  PEM_LOG_INFO(log_, "[{0:d}] attach_target_inf target:{1:d}", time(), idx);
}

void TmRingFabric::attach_target(uint32_t idx, p_tm_mem_t mem) {
  if (idx >= target_ports_.size() || mem == nullptr) {
    return;
  }
  target_ports_[idx]->attach(mem);
}

void TmRingFabric::bind_master_id(uint32_t port_id, uint32_t mst_id) {
  // Topology 和 NIU 必须同步更新，否则响应可能按旧 mst_id 返回错误端口。
  topology_->bind_master_id(port_id, mst_id);
  if (port_id < master_nius_.size()) {
    master_nius_[port_id]->set_master_id(mst_id);
  }
}

uint32_t TmRingFabric::send_rd_req(uint32_t master_port, uint64_t address,
                                   uint32_t size) {
  // UINT32_MAX 表示 API 投递失败，调用者不应把它加入待完成请求集合。
  if (master_port >= master_nius_.size()) {
    return static_cast<uint32_t>(-1);
  }
  return master_nius_[master_port]->send_rd_req(address, size);
}

uint32_t TmRingFabric::send_wr_req(uint32_t master_port, uint64_t address,
                                   uint32_t size) {
  if (master_port >= master_nius_.size()) {
    return static_cast<uint32_t>(-1);
  }
  return master_nius_[master_port]->send_wr_req(address, size);
}

bool TmRingFabric::completed(uint32_t master_port, uint32_t req_id) {
  if (master_port >= master_nius_.size()) {
    return false;
  }
  return master_nius_[master_port]->is_request_completed(req_id);
}

bool TmRingFabric::canSendRdReq(uint32_t master_port) {
  if (master_port >= master_nius_.size()) {
    return false;
  }
  return master_nius_[master_port]->can_send_rd_req();
}

bool TmRingFabric::canSendWrReq(uint32_t master_port) {
  if (master_port >= master_nius_.size()) {
    return false;
  }
  return master_nius_[master_port]->can_send_wr_req();
}

p_tm_com_inf_t TmRingFabric::get_router_port_inf(uint32_t router_id,
                                                 TmRingPortDir in_dir) const {
  return routers_[router_id]->port_inf(in_dir);
}

p_tm_ring_link_t TmRingFabric::get_ring_link(uint32_t src_router_id,
                                             TmRingPortDir src_dir,
                                             uint32_t dst_router_id,
                                             TmRingPortDir dst_dir) const {
  auto it = links_.find(
      make_link_key(src_router_id, src_dir, dst_router_id, dst_dir));
  return it == links_.end() ? nullptr : it->second;
}

uint64_t TmRingFabric::make_link_key(uint32_t src_router_id,
                                     TmRingPortDir src_dir,
                                     uint32_t dst_router_id,
                                     TmRingPortDir dst_dir) const {
  // 固定字段位置保证 EAST/WEST 以及反向 Link 生成不同的 64 位键。
  return (static_cast<uint64_t>(src_router_id) << 48) |
         (static_cast<uint64_t>(tm_ring_port_index(src_dir)) << 40) |
         (static_cast<uint64_t>(dst_router_id) << 8) |
         static_cast<uint64_t>(tm_ring_port_index(dst_dir));
}
