#include "tm_mesh_router.h"

using namespace tm_engine;

namespace {

inline uint32_t cmd_class(pld_cmd_t cmd) { return static_cast<uint32_t>(cmd); }

}  // namespace

TmMeshRouter::TmMeshRouter() {}

TmMeshRouter::TmMeshRouter(const std::string& name, p_tm_clk_t clk,
                           p_tm_mesh_cfg_t cfg)
    : TmModule(name) {
  config(name, clk, cfg);
}

TmMeshRouter::~TmMeshRouter() {}

void TmMeshRouter::config(const std::string& name, p_tm_clk_t clk,
                          p_tm_mesh_cfg_t cfg) {
  name_ = name;
  this->name(name_);
  clk_ = clk;
  cfg_ = cfg;

  req_qs_.clear();
  wr_dat_qs_.clear();
  rd_rsp_qs_.clear();
  wr_req_rsp_qs_.clear();
  wr_dat_rsp_qs_.clear();
  uint32_t rr_init_slot = tm_ring_port_count() * traffic_slot_count() - 1;
  output_rr_ptr_.assign(tm_ring_port_count() * tm_ring_subnet_count(),
                        rr_init_slot);

  for (uint32_t port = 0; port < tm_ring_port_count(); ++port) {
    req_qs_.push_back(tm_make_com_que(clk_,
                                      name_ + "_req_q_" + std::to_string(port),
                                      cfg_->ring_req_fifo_depth));

    wr_dat_qs_.push_back(
        tm_make_com_que(clk_, name_ + "_wr_dat_q_" + std::to_string(port),
                        cfg_->ring_req_fifo_depth));

    std::vector<p_tm_com_que_t> lane_qs;
    for (uint32_t lane = 0; lane < cfg_->rd_rsp_port_num; ++lane) {
      lane_qs.push_back(tm_make_com_que(clk_,
                                        name_ + "_rd_rsp_q_" +
                                            std::to_string(port) + "_" +
                                            std::to_string(lane),
                                        cfg_->ring_rsp_fifo_depth));
    }
    rd_rsp_qs_.push_back(lane_qs);

    wr_req_rsp_qs_.push_back(
        tm_make_com_que(clk_, name_ + "_wr_req_rsp_q_" + std::to_string(port),
                        cfg_->ring_rsp_fifo_depth));

    wr_dat_rsp_qs_.push_back(
        tm_make_com_que(clk_, name_ + "_wr_dat_rsp_q_" + std::to_string(port),
                        cfg_->ring_rsp_fifo_depth));
  }

  auto route_req_proc = TM_MAKE_CPROC(&TmMeshRouter::route_request);
  for (auto& q : req_qs_) {
    tm_sensitive(route_req_proc, q->vld);
  }

  for (auto& q : wr_dat_qs_) {
    tm_sensitive(route_req_proc, q->vld);
  }

  auto route_rsp_proc = TM_MAKE_CPROC(&TmMeshRouter::route_response);
  for (auto& lane_qs : rd_rsp_qs_) {
    for (auto& q : lane_qs) {
      tm_sensitive(route_rsp_proc, q->vld);
    }
  }
  for (auto& q : wr_req_rsp_qs_) {
    tm_sensitive(route_rsp_proc, q->vld);
  }
  for (auto& q : wr_dat_rsp_qs_) {
    tm_sensitive(route_rsp_proc, q->vld);
  }

  reset();
}

void TmMeshRouter::reset() {
  for (auto& q : req_qs_) {
    q->clear();
  }
  for (auto& q : wr_dat_qs_) {
    q->clear();
  }
  for (auto& lane_qs : rd_rsp_qs_) {
    for (auto& q : lane_qs) {
      q->clear();
    }
  }
  for (auto& q : wr_req_rsp_qs_) {
    q->clear();
  }
  for (auto& q : wr_dat_rsp_qs_) {
    q->clear();
  }
  uint32_t rr_init_slot = tm_ring_port_count() * traffic_slot_count() - 1;
  output_rr_ptr_.assign(tm_ring_port_count() * tm_ring_subnet_count(),
                        rr_init_slot);
}

bool TmMeshRouter::idle() const {
  bool ret = true;
  for (const auto& q : req_qs_) {
    ret = ret && q->empty();
  }
  for (const auto& q : wr_dat_qs_) {
    ret = ret && q->empty();
  }
  for (const auto& lane_qs : rd_rsp_qs_) {
    for (const auto& q : lane_qs) {
      ret = ret && q->empty();
    }
  }
  for (const auto& q : wr_req_rsp_qs_) {
    ret = ret && q->empty();
  }
  for (const auto& q : wr_dat_rsp_qs_) {
    ret = ret && q->empty();
  }
  return ret;
}

void TmMeshRouter::attach(uint32_t router_id,
                          std::shared_ptr<TmMeshTopology> topology,
                          p_tm_mesh_link_t east_link,
                          p_tm_mesh_link_t west_link,
                          TmRingLocalEndpoint* local_endpoint) {
  router_id_ = router_id;
  topology_ = topology;
  east_link_ = east_link;
  west_link_ = west_link;
  local_endpoint_ = local_endpoint;
}

void TmMeshRouter::route_request() { advance_subnet(TmRingSubnet::REQ); }

void TmMeshRouter::route_response() { advance_subnet(TmRingSubnet::RSP); }

uint32_t TmMeshRouter::traffic_slot_count() const {
  uint32_t extra_rd_rsp_lanes =
      cfg_->rd_rsp_port_num > 0 ? cfg_->rd_rsp_port_num - 1 : 0;
  return cmd_class(PldCmd::UNDEF) + extra_rd_rsp_lanes;
}

void TmMeshRouter::decode_slot(uint32_t slot_class,
                               uint32_t& traffic_class,
                               uint32_t& rsp_lane) {
  traffic_class = slot_class;
  rsp_lane = 0;

  if (slot_class >= cmd_class(PldCmd::UNDEF)) {
    traffic_class = cmd_class(PldCmd::RD_RSP);
    rsp_lane = slot_class - cmd_class(PldCmd::UNDEF) + 1;
  }
}

uint32_t TmMeshRouter::traffic_class(pld_cmd_t cmd) { return cmd_class(cmd); }

p_tm_com_que_t TmMeshRouter::req_q(TmMeshPortDir in_dir) const {
  return req_qs_[tm_ring_port_index(in_dir)];
}

p_tm_com_que_t TmMeshRouter::wr_dat_q(TmMeshPortDir in_dir) const {
  return wr_dat_qs_[tm_ring_port_index(in_dir)];
}

p_tm_com_que_t TmMeshRouter::rd_rsp_q(TmMeshPortDir in_dir,
                                      uint32_t lane) const {
  return rd_rsp_qs_[tm_ring_port_index(in_dir)][lane];
}

p_tm_com_que_t TmMeshRouter::wr_req_rsp_q(TmMeshPortDir in_dir) const {
  return wr_req_rsp_qs_[tm_ring_port_index(in_dir)];
}

p_tm_com_que_t TmMeshRouter::wr_dat_rsp_q(TmMeshPortDir in_dir) const {
  return wr_dat_rsp_qs_[tm_ring_port_index(in_dir)];
}

p_tm_com_que_t TmMeshRouter::queue_for_class(TmMeshPortDir in_dir,
                                             uint32_t traffic_class,
                                             uint32_t lane) const {
  uint32_t port = tm_ring_port_index(in_dir);
  auto cmd = static_cast<PldCmd>(traffic_class);
  if (cmd == PldCmd::RD || cmd == PldCmd::WR) {
    return req_qs_[port];
  }
  if (cmd == PldCmd::WR_DAT) {
    return wr_dat_qs_[port];
  }
  if (cmd == PldCmd::WR_RSP) {
    return wr_req_rsp_qs_[port];
  }
  if (cmd == PldCmd::RSP) {
    return wr_dat_rsp_qs_[port];
  }
  return rd_rsp_qs_[port][lane];
}
/*
确定 EAST+REQ 对应的仲裁器
        ↓
从上次获胜位置的下一个 slot 开始扫描
        ↓
把 slot 转换成：输入端口 + 命令类型 + RD_RSP lane
        ↓
过滤掉不属于当前 REQ/RSP subnet 的队列
        ↓
检查队列是否有数据
        ↓
计算这个包应该走 LOCAL/EAST/WEST
        ↓
检查是否正好走当前 out_dir
        ↓
检查下游 Router/Link/Target 是否可接收
        ↓
找到第一个满足条件的包，返回 winner
*/
TmRingSubnet TmMeshRouter::packet_subnet(p_tm_pld_t pld) const {
  auto cmd = static_cast<PldCmd>(pld->ring_traffic_class);
  if (cmd == PldCmd::RD || cmd == PldCmd::WR || cmd == PldCmd::WR_DAT) {
    return TmRingSubnet::REQ;
  }
  return TmRingSubnet::RSP;
}

p_tm_mesh_link_t TmMeshRouter::output_link(TmMeshPortDir out_dir) const {
  if (out_dir == TmMeshPortDir::EAST) {
    return east_link_;
  }
  if (out_dir == TmMeshPortDir::WEST) {
    return west_link_;
  }
  return nullptr;
}
//   每一跳根据当前 router_id 和最终 destination 算 EAST / WEST / LOCAL
void TmMeshRouter::resolve_route(p_tm_pld_t pld) {
  auto cmd = static_cast<PldCmd>(pld->ring_traffic_class);
  bool is_request = cmd == PldCmd::RD || cmd == PldCmd::WR ||
                    cmd == PldCmd::WR_DAT;
  uint32_t destination =
      is_request ? tm_pld_dst_node(pld) : tm_pld_src_node(pld);
  auto out_dir = router_id_ == destination
                     ? TmMeshPortDir::LOCAL
                     : topology_->route_direction(router_id_, destination);
  pld->ring_out_dir = tm_ring_port_index(out_dir);
}
//如果下一跳是 EAST/WEST，检查 link 能不能发
// 如果下一跳是 LOCAL，调用 Fabric::can_accept_local()
bool TmMeshRouter::route_ready(p_tm_pld_t pld) {
  auto out_dir = static_cast<TmMeshPortDir>(pld->ring_out_dir);
  if (out_dir == TmMeshPortDir::LOCAL) {
    return local_endpoint_->can_accept_local(pld);
  }

  auto link = output_link(out_dir);
  return link != nullptr && link->can_send(packet_subnet(pld), time());
}

bool TmMeshRouter::route_packet(p_tm_pld_t pld) {
  auto out_dir = static_cast<TmMeshPortDir>(pld->ring_out_dir);
  if (out_dir == TmMeshPortDir::LOCAL) {
    return local_endpoint_->accept_local(pld);
  }

  auto link = output_link(out_dir);
  if (link == nullptr || !link->can_send(packet_subnet(pld), time())) {
    return false;
  }
  link->enqueue(packet_subnet(pld), pld, pld->ring_traffic_class, time());
  return true;
}

bool TmMeshRouter::select_output_candidate(TmMeshPortDir out_dir,
                                           TmRingSubnet subnet,
                                           p_tm_pld_t& winner) {
  uint32_t class_num = traffic_slot_count();
  uint32_t slot_count = tm_ring_port_count() * class_num;
  uint32_t subnet_idx = tm_ring_subnet_index(subnet);
  uint32_t out_idx =
      subnet_idx * tm_ring_port_count() + tm_ring_port_index(out_dir);
  auto& last_grant_slot = output_rr_ptr_[out_idx];
  if (last_grant_slot >= slot_count) {
    last_grant_slot = slot_count - 1;
  }

  for (uint32_t offset = 1; offset <= slot_count; ++offset) {
    uint32_t slot = (last_grant_slot + offset) % slot_count;
    uint32_t port = slot / class_num;
    uint32_t slot_class = slot % class_num;
    uint32_t cls;
    uint32_t lane;
    decode_slot(slot_class, cls, lane);

    auto in_dir = static_cast<TmMeshPortDir>(port);
    auto cmd = static_cast<PldCmd>(cls);
    bool is_req = cmd == PldCmd::RD || cmd == PldCmd::WR ||
                  cmd == PldCmd::WR_DAT;
    if ((subnet == TmRingSubnet::REQ) != is_req) {
      continue;
    }

    auto q = queue_for_class(in_dir, cls, lane);
    if (q->empty()) {
      continue;
    }
    auto pld = q->front();

    pld->ring_in_dir = port;
    pld->ring_traffic_class = cls;
    pld->ring_rsp_lane = lane;

    resolve_route(pld);
    if (static_cast<TmMeshPortDir>(pld->ring_out_dir) != out_dir ||
        !route_ready(pld)) {
      continue;
    }

    winner = pld;
    return true;
  }

  return false;
}

void TmMeshRouter::advance_subnet(TmRingSubnet subnet) {
  for (uint32_t port = 0; port < tm_ring_port_count(); ++port) {
    auto out_dir = static_cast<TmMeshPortDir>(port);
    p_tm_pld_t winner = nullptr;
    if (!select_output_candidate(out_dir, subnet, winner)) {
      continue;
    }

    if (route_packet(winner)) {
      uint32_t subnet_idx = tm_ring_subnet_index(subnet);
      uint32_t out_idx =
          subnet_idx * tm_ring_port_count() + tm_ring_port_index(out_dir);
      output_rr_ptr_[out_idx] =
          winner->ring_in_dir * traffic_slot_count() +
          (winner->ring_traffic_class == cmd_class(PldCmd::RD_RSP)
               ? (winner->ring_rsp_lane == 0
                      ? cmd_class(PldCmd::RD_RSP)
                      : cmd_class(PldCmd::UNDEF) + winner->ring_rsp_lane - 1)
               : winner->ring_traffic_class);
      queue_for_class(static_cast<TmMeshPortDir>(winner->ring_in_dir),
                      winner->ring_traffic_class, winner->ring_rsp_lane)
          ->pop_front();
    }
  }
}
