#include "tm_ring_router.h"

#include <algorithm>

#include "tm_pld.h"
#include "tm_ring.h"
#include "tm_ring_inf.h"
#include "tm_ring_target_port.h"

using namespace tm_engine;

namespace {

inline uint32_t cmd_class(pld_cmd_t cmd) { return static_cast<uint32_t>(cmd); }

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

  req_infs_.clear();
  wr_dat_infs_.clear();
  rd_rsp_infs_.clear();
  wr_req_rsp_infs_.clear();
  wr_dat_rsp_infs_.clear();
  uint32_t rr_init_slot = tm_ring_port_count() * traffic_slot_count() - 1;
  output_rr_ptr_.assign(tm_ring_port_count() * tm_ring_subnet_count(),
                        rr_init_slot);
  uint32_t router_input_depth =
      std::max<uint32_t>(1, cfg_->ring_router_input_depth);

  for (uint32_t port = 0; port < tm_ring_port_count(); ++port) {
    req_infs_.push_back(tm_make_com_inf(
        clk_, name_ + "_req_inf_" + std::to_string(port),
        router_input_depth));

    wr_dat_infs_.push_back(tm_make_com_inf(
        clk_, name_ + "_wr_dat_inf_" + std::to_string(port),
        router_input_depth));

    std::vector<p_tm_com_inf_t> lane_infs;
    for (uint32_t lane = 0; lane < cfg_->rd_rsp_port_num; ++lane) {
      lane_infs.push_back(tm_make_com_inf(
          clk_,
          name_ + "_rd_rsp_inf_" + std::to_string(port) + "_" +
              std::to_string(lane),
          router_input_depth));
    }
    rd_rsp_infs_.push_back(lane_infs);

    wr_req_rsp_infs_.push_back(tm_make_com_inf(
        clk_, name_ + "_wr_req_rsp_inf_" + std::to_string(port),
        router_input_depth));

    wr_dat_rsp_infs_.push_back(tm_make_com_inf(
        clk_, name_ + "_wr_dat_rsp_inf_" + std::to_string(port),
        router_input_depth));
  }

  auto route_req_proc = TM_MAKE_CPROC(&TmRingRouter::route_request);
  for (auto& inf : req_infs_) {
    tm_sensitive(route_req_proc, inf->vld);
  }

  for (auto& inf : wr_dat_infs_) {
    tm_sensitive(route_req_proc, inf->vld);
  }

  auto route_rsp_proc = TM_MAKE_CPROC(&TmRingRouter::route_response);
  for (auto& lane_infs : rd_rsp_infs_) {
    for (auto& inf : lane_infs) {
      tm_sensitive(route_rsp_proc, inf->vld);
    }
  }
  for (auto& inf : wr_req_rsp_infs_) {
    tm_sensitive(route_rsp_proc, inf->vld);
  }
  for (auto& inf : wr_dat_rsp_infs_) {
    tm_sensitive(route_rsp_proc, inf->vld);
  }

  reset();
}

void TmRingRouter::reset() {
  for (auto& inf : req_infs_) {
    inf->reset();
  }
  for (auto& inf : wr_dat_infs_) {
    inf->reset();
  }
  for (auto& lane_infs : rd_rsp_infs_) {
    for (auto& inf : lane_infs) {
      inf->reset();
    }
  }
  for (auto& inf : wr_req_rsp_infs_) {
    inf->reset();
  }
  for (auto& inf : wr_dat_rsp_infs_) {
    inf->reset();
  }
  uint32_t rr_init_slot = tm_ring_port_count() * traffic_slot_count() - 1;
  output_rr_ptr_.assign(tm_ring_port_count() * tm_ring_subnet_count(),
                        rr_init_slot);
}

bool TmRingRouter::idle() const {
  bool ret = true;
  for (const auto& inf : req_infs_) {
    ret = ret && inf->idle();
  }
  for (const auto& inf : wr_dat_infs_) {
    ret = ret && inf->idle();
  }
  for (const auto& lane_infs : rd_rsp_infs_) {
    for (const auto& inf : lane_infs) {
      ret = ret && inf->idle();
    }
  }
  for (const auto& inf : wr_req_rsp_infs_) {
    ret = ret && inf->idle();
  }
  for (const auto& inf : wr_dat_rsp_infs_) {
    ret = ret && inf->idle();
  }
  return ret;
}

void TmRingRouter::attach(uint32_t router_id,
                          std::shared_ptr<TmRingTopology> topology,
                          p_tm_ring_link_t east_link,
                          p_tm_ring_link_t west_link,
                          std::vector<std::shared_ptr<TmRingInf>>* master_nius,
                          std::vector<std::shared_ptr<TmRingTargetPort>>*
                              target_ports,
                          TmRingFabric* fabric) {
  router_id_ = router_id;
  topology_ = topology;
  east_link_ = east_link;
  west_link_ = west_link;
  master_nius_ = master_nius;
  target_ports_ = target_ports;
  fabric_ = fabric;
}

void TmRingRouter::route_request() { advance_subnet(TmRingSubnet::REQ); }

void TmRingRouter::route_response() { advance_subnet(TmRingSubnet::RSP); }

uint32_t TmRingRouter::traffic_slot_count() const {
  uint32_t extra_rd_rsp_lanes =
      cfg_->rd_rsp_port_num > 0 ? cfg_->rd_rsp_port_num - 1 : 0;
  return cmd_class(PldCmd::UNDEF) + extra_rd_rsp_lanes;
}

void TmRingRouter::decode_slot(uint32_t slot_class, uint32_t& traffic_class,
                               uint32_t& rsp_lane) {
  traffic_class = slot_class;
  rsp_lane = 0;

  if (slot_class >= cmd_class(PldCmd::UNDEF)) {
    traffic_class = cmd_class(PldCmd::RD_RSP);
    rsp_lane = slot_class - cmd_class(PldCmd::UNDEF) + 1;
  }
}

p_tm_com_inf_t TmRingRouter::req_inf(TmRingPortDir in_dir) const {
  return req_infs_[tm_ring_port_index(in_dir)];
}

p_tm_com_inf_t TmRingRouter::wr_dat_inf(TmRingPortDir in_dir) const {
  return wr_dat_infs_[tm_ring_port_index(in_dir)];
}

p_tm_com_inf_t TmRingRouter::rd_rsp_inf(TmRingPortDir in_dir,
                                        uint32_t lane) const {
  return rd_rsp_infs_[tm_ring_port_index(in_dir)][lane];
}

p_tm_com_inf_t TmRingRouter::wr_req_rsp_inf(TmRingPortDir in_dir) const {
  return wr_req_rsp_infs_[tm_ring_port_index(in_dir)];
}

p_tm_com_inf_t TmRingRouter::wr_dat_rsp_inf(TmRingPortDir in_dir) const {
  return wr_dat_rsp_infs_[tm_ring_port_index(in_dir)];
}

p_tm_com_inf_t TmRingRouter::inf_for_class(TmRingPortDir in_dir,
                                             uint32_t traffic_class,
                                             uint32_t lane) const {
  uint32_t port = tm_ring_port_index(in_dir);
  auto cmd = static_cast<PldCmd>(traffic_class);
  if (cmd == PldCmd::RD || cmd == PldCmd::WR) {
    return req_infs_[port];
  }
  if (cmd == PldCmd::WR_DAT) {
    return wr_dat_infs_[port];
  }
  if (cmd == PldCmd::WR_RSP) {
    return wr_req_rsp_infs_[port];
  }
  if (cmd == PldCmd::RSP) {
    return wr_dat_rsp_infs_[port];
  }
  return rd_rsp_infs_[port][lane];
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

void TmRingRouter::resolve_route(p_tm_pld_t pld) {
  auto cmd = static_cast<PldCmd>(pld->ring_traffic_class);
  bool is_request =
      cmd == PldCmd::RD || cmd == PldCmd::WR || cmd == PldCmd::WR_DAT;
  uint32_t destination =
      is_request ? tm_pld_dst_node(pld) : tm_pld_src_node(pld);
  auto out_dir = router_id_ == destination
                     ? TmRingPortDir::LOCAL
                     : topology_->route_direction(router_id_, destination);
  pld->ring_out_dir = tm_ring_port_index(out_dir);
}

bool TmRingRouter::route_ready(p_tm_pld_t pld) {
  auto out_dir = static_cast<TmRingPortDir>(pld->ring_out_dir);
  if (out_dir == TmRingPortDir::LOCAL) {
    return local_ready(pld);
  }

  auto link = output_link(out_dir);
  return link != nullptr && link->can_send(pld);
}

bool TmRingRouter::route_packet(p_tm_pld_t pld) {
  auto out_dir = static_cast<TmRingPortDir>(pld->ring_out_dir);
  if (out_dir == TmRingPortDir::LOCAL) {
    return route_local(pld);
  }

  auto link = output_link(out_dir);
  if (link == nullptr || !link->can_send(pld)) {
    return false;
  }
  link->enqueue(pld);
  return true;
}

std::shared_ptr<TmRingInf> TmRingRouter::local_master(p_tm_pld_t pld) const {
  uint32_t master_port = topology_->find_master_port(pld->mst_id);
  if (master_nius_ == nullptr || master_port >= master_nius_->size()) {
    return nullptr;
  }
  return (*master_nius_)[master_port];
}

std::shared_ptr<TmRingTargetPort> TmRingRouter::local_target(
    p_tm_pld_t pld) const {
  uint32_t target_id = tm_pld_target_id(pld);
  if (target_ports_ == nullptr || target_id >= target_ports_->size()) {
    return nullptr;
  }
  return (*target_ports_)[target_id];
}

bool TmRingRouter::local_ready(p_tm_pld_t pld) {
  auto cmd = static_cast<PldCmd>(pld->ring_traffic_class);
  if (tm_ring_is_req_cmd(cmd)) {
    auto target = local_target(pld);
    return target != nullptr && target->can_accept_request(cmd);
  }

  auto niu = local_master(pld);
  return niu != nullptr && niu->can_accept_rsp(pld);
}

bool TmRingRouter::route_local(p_tm_pld_t pld) {
  auto cmd = static_cast<PldCmd>(pld->ring_traffic_class);
  if (tm_ring_is_req_cmd(cmd)) {
    auto target = local_target(pld);
    if (target == nullptr || !target->can_accept_request(cmd)) {
      return false;
    }
    target->accept_request(cmd, pld);
    return true;
  }

  auto niu = local_master(pld);
  if (niu == nullptr || !niu->recv_rsp(pld)) {
    return false;
  }
  fabric_->complete_master_response(pld);
  return true;
}

p_tm_pld_t TmRingRouter::select_output_candidate(TmRingPortDir out_dir,
                                                 TmRingSubnet subnet) {
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

    auto in_dir = static_cast<TmRingPortDir>(port);
    auto cmd = static_cast<PldCmd>(cls);
    bool is_req =
        cmd == PldCmd::RD || cmd == PldCmd::WR || cmd == PldCmd::WR_DAT;
    if ((subnet == TmRingSubnet::REQ) != is_req) {
      continue;
    }

    auto inf = inf_for_class(in_dir, cls, lane);
    if (!inf->valid()) {
      continue;
    }
    auto pld = inf->get_pld();

    pld->ring_in_dir = port;
    pld->ring_subnet = tm_ring_subnet_index(subnet);
    pld->ring_traffic_class = cls;
    pld->ring_rsp_lane = lane;

    resolve_route(pld);
    if (static_cast<TmRingPortDir>(pld->ring_out_dir) != out_dir ||
        !route_ready(pld)) {
      continue;
    }

    return pld;
  }

  return nullptr;
}

void TmRingRouter::advance_subnet(TmRingSubnet subnet) {
  for (uint32_t port = 0; port < tm_ring_port_count(); ++port) {
    auto out_dir = static_cast<TmRingPortDir>(port);
    auto winner = select_output_candidate(out_dir, subnet);
    if (winner == nullptr) {
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
      inf_for_class(static_cast<TmRingPortDir>(winner->ring_in_dir),
                    winner->ring_traffic_class, winner->ring_rsp_lane)
          ->pop_pld();
    }
  }
}
