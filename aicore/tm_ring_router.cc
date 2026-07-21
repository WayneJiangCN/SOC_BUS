#include "tm_ring_router.h"

#include <algorithm>

#include "tm_pld.h"

using namespace tm_engine;

namespace {

inline uint32_t cmd_class(pld_cmd_t cmd) { return static_cast<uint32_t>(cmd); }

inline uint32_t ring_port_slot(TmRingPortDir dir)
{
    // port_infs_ 只保存外部 EAST/WEST 两个方向，因此使用紧凑的 0/1 下标。
    return dir == TmRingPortDir::EAST ? 0 : 1;
}

inline TmRingPortDir ring_port_dir(uint32_t slot)
{
    return slot == 0 ? TmRingPortDir::EAST : TmRingPortDir::WEST;
}

}  // namespace

TmRingRouter::TmRingRouter() {}

TmRingRouter::TmRingRouter(const std::string& name, p_tm_clk_t clk,
                           p_tm_ring_cfg_t cfg)
    : TmModule(name) {
  config(name, clk, cfg);
}

TmRingRouter::~TmRingRouter() {}

void TmRingRouter::config(const std::string& name, p_tm_clk_t clk,
                          p_tm_ring_cfg_t cfg) {
  name_ = name;
  this->name(name_);
  clk_ = clk;
  cfg_ = cfg;
  log_para_t log_para(name_ + ".log");
  log_ = pem_log::create_logger(log_para);
  PEM_LOG_INFO(log_, "[{0:d}] config", time());

  port_infs_.clear();
  local_master_infs_.clear();
  local_target_infs_.clear();
  // RR 初值放在最后一个 slot，使首次扫描从 slot0 开始。
  uint32_t rr_init_slot = tm_ring_port_count() * traffic_slot_count() - 1;
  output_rr_ptr_.assign(tm_ring_port_count() * tm_ring_subnet_count(),
                        rr_init_slot);
  input_rr_ptr_.assign(tm_ring_port_count() * tm_ring_subnet_count(),
                       traffic_slot_count() - 1);
  uint32_t chan_num = tm_ring_packet_channel_count(cfg_->rd_rsp_port_num);

  // EAST/WEST 端口接收相邻 Link 的包；LOCAL 端口通过独立 Master/Target 接口表达。
  for (uint32_t port = 0; port < 2; ++port) {
    auto dir = ring_port_dir(port);
    auto inf = tm_make_com_inf(
        clk_, name_ + "_port_inf_" +
                  std::to_string(tm_ring_port_index(dir)),
        cfg_->master_inf_delay + 1);
    inf->set_chan_num(chan_num);
    port_infs_.push_back(inf);
  }

  // 一个方向的 vld 同时可能来自 REQ 或 RSP 通道，各回调只扫描所属子网。
  tm_sensitive(TM_MAKE_CPROC(&TmRingRouter::route_east_request),
               port_infs_[ring_port_slot(TmRingPortDir::EAST)]->vld);
  tm_sensitive(TM_MAKE_CPROC(&TmRingRouter::route_east_response),
               port_infs_[ring_port_slot(TmRingPortDir::EAST)]->vld);
  tm_sensitive(TM_MAKE_CPROC(&TmRingRouter::route_west_request),
               port_infs_[ring_port_slot(TmRingPortDir::WEST)]->vld);
  tm_sensitive(TM_MAKE_CPROC(&TmRingRouter::route_west_response),
               port_infs_[ring_port_slot(TmRingPortDir::WEST)]->vld);

  reset();
}

void TmRingRouter::reset() {
  // 清空端口握手状态，防止 reset 前的在途 payload 在 reset 后继续转发。
  for (auto& inf : port_infs_) {
    inf->reset();
  }
  for (auto& inf : local_master_infs_) {
    if (inf != nullptr) {
      inf->reset();
    }
  }
  for (auto& inf : local_target_infs_) {
    if (inf != nullptr) {
      inf->reset();
    }
  }
  // 仲裁指针恢复后，下一次 grant 从固定 slot0 开始，保证测试可重复。
  uint32_t rr_init_slot = tm_ring_port_count() * traffic_slot_count() - 1;
  output_rr_ptr_.assign(tm_ring_port_count() * tm_ring_subnet_count(),
                        rr_init_slot);
  input_rr_ptr_.assign(tm_ring_port_count() * tm_ring_subnet_count(),
                       traffic_slot_count() - 1);
}

bool TmRingRouter::idle() const {
  // Router 不保存完整事务状态，空闲只取决于所有输入/LOCAL 接口是否清空。
  bool ret = true;
  for (const auto& inf : port_infs_) {
    ret = ret && inf->idle();
  }
  for (const auto& inf : local_master_infs_) {
    ret = ret && (inf == nullptr || inf->idle());
  }
  for (const auto& inf : local_target_infs_) {
    ret = ret && (inf == nullptr || inf->idle());
  }
  return ret;
}

void TmRingRouter::attach(uint32_t router_id,
                          std::shared_ptr<TmRingTopology> topology,
                          p_tm_ring_link_t east_link,
                          p_tm_ring_link_t west_link) {
  // Router 仅持有本节点所需对象，不反向依赖整个 Fabric，降低模块耦合。
  router_id_ = router_id;
  topology_ = topology;
  east_link_ = east_link;
  west_link_ = west_link;
  if (east_link_ != nullptr) {
    PEM_LOG_INFO(log_, "[{0:d}] attach_east_link router:{1:d}",
                 time(), router_id_);
  }
  if (west_link_ != nullptr) {
    PEM_LOG_INFO(log_, "[{0:d}] attach_west_link router:{1:d}",
                 time(), router_id_);
  }
}

void TmRingRouter::bind_local_master(uint32_t master_port,
                                     p_tm_com_inf_t inf) {
  // vector 下标就是 Master port，允许一个 Router ring stop 挂接多个 Master。
  if (master_port >= local_master_infs_.size()) {
    local_master_infs_.resize(master_port + 1, nullptr);
  }
  if (local_master_infs_[master_port] == nullptr) {
    local_master_infs_[master_port] = tm_make_com_inf(
        clk_, name_ + "_local_master_inf_" + std::to_string(master_port),
        cfg_->master_inf_delay + 1);
    local_master_infs_[master_port]->set_chan_num(
        tm_ring_packet_channel_count(cfg_->rd_rsp_port_num));
    // LOCAL Master 只注入请求，因此其 vld 绑定请求子网处理函数。
    tm_sensitive(TM_MAKE_CPROC(&TmRingRouter::route_local_request),
                 local_master_infs_[master_port]->vld);
  }
  local_master_infs_[master_port]->connect(inf);
  PEM_LOG_INFO(log_, "[{0:d}] bind_local_master port:{1:d}",
               time(), master_port);
}

void TmRingRouter::bind_local_target(uint32_t target_id,
                                     p_tm_com_inf_t inf) {
  // vector 下标就是 Target ID，响应弹出时可直接根据 slv_id 定位端口。
  if (target_id >= local_target_infs_.size()) {
    local_target_infs_.resize(target_id + 1, nullptr);
  }
  if (local_target_infs_[target_id] == nullptr) {
    local_target_infs_[target_id] = tm_make_com_inf(
        clk_, name_ + "_local_target_inf_" + std::to_string(target_id),
        cfg_->target_inf_delay + 1);
    local_target_infs_[target_id]->set_chan_num(
        tm_ring_packet_channel_count(cfg_->rd_rsp_port_num));
    // LOCAL Target 只向 Ring 注入响应，因此其 vld 绑定响应子网处理函数。
    tm_sensitive(TM_MAKE_CPROC(&TmRingRouter::route_local_response),
                 local_target_infs_[target_id]->vld);
  }
  local_target_infs_[target_id]->connect(inf);
  PEM_LOG_INFO(log_, "[{0:d}] bind_local_target target:{1:d}",
               time(), target_id);
}

void TmRingRouter::route_local_request() {
  advance_local_input(TmRingSubnet::REQ);
}

void TmRingRouter::route_east_request() {
  advance_east_input(TmRingSubnet::REQ);
}

void TmRingRouter::route_west_request() {
  advance_west_input(TmRingSubnet::REQ);
}

void TmRingRouter::route_local_response() {
  advance_local_input(TmRingSubnet::RSP);
}

void TmRingRouter::route_east_response() {
  advance_east_input(TmRingSubnet::RSP);
}

void TmRingRouter::route_west_response() {
  advance_west_input(TmRingSubnet::RSP);
}

uint32_t TmRingRouter::traffic_slot_count() const {
  // 基础命令各占一个 slot，RD_RSP 的额外 lane 各追加一个独立仲裁 slot。
  uint32_t extra_rd_rsp_lanes =
      cfg_->rd_rsp_port_num > 0 ? cfg_->rd_rsp_port_num - 1 : 0;
  return tm_ring_base_packet_channel_count() + extra_rd_rsp_lanes;
}

void TmRingRouter::decode_slot(uint32_t slot_class, uint32_t& traffic_class,
                               uint32_t& rsp_lane) {
  traffic_class = slot_class;
  rsp_lane = 0;

  // 额外 slot 仍属于 RD_RSP traffic class，仅 lane 编号不同。
  if (slot_class >= tm_ring_base_packet_channel_count()) {
    traffic_class = cmd_class(PldCmd::RD_RSP);
    rsp_lane = slot_class - tm_ring_base_packet_channel_count() + 1;
  }
}

p_tm_com_inf_t TmRingRouter::port_inf(TmRingPortDir in_dir) const {
  return port_infs_[ring_port_slot(in_dir)];
}

uint32_t TmRingRouter::packet_channel(uint32_t traffic_class,
                                      uint32_t lane) const {
  auto cmd = static_cast<PldCmd>(traffic_class);
  return tm_ring_packet_channel(cmd, lane);
}

p_tm_com_inf_t TmRingRouter::inf_for_class(TmRingPortDir in_dir,
                                           uint32_t traffic_class,
                                           uint32_t lane) const {
  // 相邻 Link 的所有 traffic class 复用同一个方向端口，通过 channel 区分。
  if (in_dir != TmRingPortDir::LOCAL) {
    return port_infs_[ring_port_slot(in_dir)];
  }

  uint32_t chan = packet_channel(traffic_class, lane);
  auto cmd = static_cast<PldCmd>(traffic_class);
  // LOCAL 请求只能来自 Master；返回首个在对应通道上有效的本地接口。
  if (tm_ring_is_req_cmd(cmd)) {
    for (const auto& inf : local_master_infs_) {
      if (inf != nullptr && inf->valid(chan)) {
        return inf;
      }
    }
    return nullptr;
  }

  // LOCAL 响应只能来自 TargetPort。
  for (const auto& inf : local_target_infs_) {
    if (inf != nullptr && inf->valid(chan)) {
      return inf;
    }
  }
  return nullptr;
}

p_tm_ring_link_t TmRingRouter::output_link(TmRingPortDir out_dir) const {
  if (out_dir == TmRingPortDir::EAST) {
    return east_link_;
  }
  if (out_dir == TmRingPortDir::WEST) {
    return west_link_;
  }
  return nullptr;
}

TmRingPortDir TmRingRouter::resolve_route(p_tm_pld_t pld) {
  auto cmd = static_cast<PldCmd>(pld->ring_traffic_class);
  bool is_request =
      cmd == PldCmd::RD || cmd == PldCmd::WR || cmd == PldCmd::WR_DAT;
  // 请求以 Target 节点为目的地，响应反向以原 Master 节点为目的地。
  uint32_t destination =
      is_request ? tm_pld_dst_node(pld) : tm_pld_src_node(pld);
  return router_id_ == destination
             ? TmRingPortDir::LOCAL
             : topology_->route_direction(router_id_, destination);
}

bool TmRingRouter::route_ready(p_tm_ring_candidate_t candidate) {
  // LOCAL 检查目的端口是否存在；跨节点则检查对应 Link 的带宽和在途容量。
  auto out_dir = candidate->out_dir;
  if (out_dir == TmRingPortDir::LOCAL) {
    return local_ready(candidate->pld);
  }

  auto link = output_link(out_dir);
  if (link == nullptr) {
    return false;
  }
  if (candidate->in_dir == TmRingPortDir::LOCAL) {
    return link->can_accept_preserving_bubble(candidate->pld);
  }
  return link->can_accept(candidate->pld);
}

bool TmRingRouter::route_packet(p_tm_ring_candidate_t candidate) {
  // 本地弹出使用 TmInf 握手，跨节点转发直接进入 Link 的序列化/延迟模型。
  auto pld = candidate->pld;
  auto out_dir = candidate->out_dir;
  if (out_dir == TmRingPortDir::LOCAL) {
    return route_local(pld);
  }

  auto link = output_link(out_dir);
  if (link == nullptr) {
    return false;
  }
  if (candidate->in_dir == TmRingPortDir::LOCAL) {
    return link->accept_pkt_preserving_bubble(pld);
  }
  return link->accept_pkt(pld);
}

bool TmRingRouter::local_ready(p_tm_pld_t pld) {
  return local_inf(pld) != nullptr;
}

bool TmRingRouter::route_local(p_tm_pld_t pld) {
  auto inf = local_inf(pld);
  if (inf == nullptr) {
    return false;
  }

  // send 失败说明目的 NIU/TargetPort 暂不可接收，输入端 payload 保持不动。
  if (!inf->send(local_channel(pld), pld)) {
    return false;
  }

  return true;
}

p_tm_com_inf_t TmRingRouter::local_inf(p_tm_pld_t pld) const {
  auto cmd = static_cast<PldCmd>(pld->ring_traffic_class);
  // 请求根据 slv_id 找 Target；响应根据 mst_id 反查 Master port。
  if (tm_ring_is_req_cmd(cmd)) {
    uint32_t target_id = tm_pld_target_id(pld);
    if (target_id >= local_target_infs_.size()) {
      return nullptr;
    }
    return local_target_infs_[target_id];
  }

  uint32_t master_port = topology_->find_master_port(pld->mst_id);
  if (master_port >= local_master_infs_.size()) {
    return nullptr;
  }
  return local_master_infs_[master_port];
}

uint32_t TmRingRouter::local_channel(p_tm_pld_t pld) const {
  return tm_ring_packet_channel(static_cast<PldCmd>(pld->ring_traffic_class),
                                pld->ring_rsp_lane);
}
p_tm_ring_candidate_t TmRingRouter::select_input_candidate(TmRingPortDir in_dir,
                                                           TmRingSubnet subnet) {
  // 每次只为一个输入方向选择一个候选，防止同一输入在一次事件中被重复消费。
  auto candidate = std::make_shared<tm_ring_candidate_t>();
  uint32_t class_num = traffic_slot_count();
  uint32_t subnet_idx = tm_ring_subnet_index(subnet);
  uint32_t in_idx =
      subnet_idx * tm_ring_port_count() + tm_ring_port_index(in_dir);
  // 从上次 grant 的下一个稳定 slot 开始扫描，候选变化不会破坏 RR 公平性。
  uint32_t last_slot =
      in_idx < input_rr_ptr_.size() ? input_rr_ptr_[in_idx] : class_num - 1;
  for (uint32_t offset = 1; offset <= class_num; ++offset) {
    uint32_t slot_class = (last_slot + offset) % class_num;
    uint32_t cls;
    uint32_t lane;
    decode_slot(slot_class, cls, lane);

    auto cmd = static_cast<PldCmd>(cls);
    bool is_req =
        cmd == PldCmd::RD || cmd == PldCmd::WR || cmd == PldCmd::WR_DAT;
    // 请求和响应共享物理端口，但只在本次指定的逻辑子网中参与仲裁。
    if ((subnet == TmRingSubnet::REQ) != is_req) {
      continue;
    }

    auto inf = inf_for_class(in_dir, cls, lane);
    if (inf == nullptr) {
      continue;
    }
    uint32_t chan = packet_channel(cls, lane);
    if (!inf->valid(chan)) {
      continue;
    }
    auto pld = inf->get_pld(chan);

    // candidate 保存本跳局部方向，不修改 payload 中稳定的事务元数据。
    candidate->pld = pld;
    candidate->in_dir = in_dir;
    candidate->out_dir = resolve_route(pld);
    candidate->slot_id = tm_ring_port_index(in_dir) * class_num + slot_class;

    // 下游不可接收时继续找其他可发送 slot，不弹出当前输入事务。
    if (!route_ready(candidate)) {
      candidate->pld = nullptr;
      continue;
    }

    return candidate;
  }

  return nullptr;
}

void TmRingRouter::advance_local_input(TmRingSubnet subnet) {
  advance_input(TmRingPortDir::LOCAL, subnet);
}

void TmRingRouter::advance_east_input(TmRingSubnet subnet) {
  advance_input(TmRingPortDir::EAST, subnet);
}

void TmRingRouter::advance_west_input(TmRingSubnet subnet) {
  advance_input(TmRingPortDir::WEST, subnet);
}

void TmRingRouter::advance_input(TmRingPortDir in_dir, TmRingSubnet subnet) {
  auto winner = select_input_candidate(in_dir, subnet);
  if (winner == nullptr) {
    return;
  }

  // 先成功送入 LOCAL 或 Link，再提交仲裁并消费输入，保证事务不丢失。
  if (route_packet(winner)) {
    commit_packet(subnet, winner);
  }
}

void TmRingRouter::commit_packet(TmRingSubnet subnet,
                                 p_tm_ring_candidate_t candidate) {
  auto pld = candidate->pld;
  uint32_t subnet_idx = tm_ring_subnet_index(subnet);
  uint32_t in_idx =
      subnet_idx * tm_ring_port_count() + tm_ring_port_index(candidate->in_dir);
  uint32_t out_idx =
      subnet_idx * tm_ring_port_count() + tm_ring_port_index(candidate->out_dir);
  // 输入 RR 决定同一输入上的 traffic class 公平性，输出 RR 保留输出获胜记录。
  if (in_idx < input_rr_ptr_.size()) {
    input_rr_ptr_[in_idx] = candidate->slot_id % traffic_slot_count();
  }
  output_rr_ptr_[out_idx] = candidate->slot_id;

  // pop 是事务离开当前 Router 的唯一位置，必须晚于下游 send 成功。
  auto in_inf = inf_for_class(candidate->in_dir, pld->ring_traffic_class,
                              pld->ring_rsp_lane);
  in_inf->pop_pld(packet_channel(pld->ring_traffic_class, pld->ring_rsp_lane));
  PEM_LOG_INFO(log_, "[{0:d}] route_commit router:{1:d} subnet:{2:d} "
                     "cmd:{3:d} gid:{4:d} in:{5:d} out:{6:d} addr:0x{7:x}",
               time(), router_id_, tm_ring_subnet_index(subnet),
               pld->ring_traffic_class, pld->gid,
               tm_ring_port_index(candidate->in_dir),
               tm_ring_port_index(candidate->out_dir), pld->addr);
}
