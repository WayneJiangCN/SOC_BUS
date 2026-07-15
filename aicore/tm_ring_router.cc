#include "tm_ring_router.h"

#include <algorithm>

#include "tm_pld.h"

using namespace tm_engine;

namespace {

inline uint32_t cmd_class(pld_cmd_t cmd) { return static_cast<uint32_t>(cmd); }

inline uint32_t ring_port_slot(TmRingPortDir dir)
{
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

  port_infs_.clear();
  local_master_infs_.clear();
  local_target_infs_.clear();
  uint32_t rr_init_slot = tm_ring_port_count() * traffic_slot_count() - 1;
  output_rr_ptr_.assign(tm_ring_port_count() * tm_ring_subnet_count(),
                        rr_init_slot);
  uint32_t router_input_depth =
      std::max<uint32_t>(1, cfg_->ring_router_input_depth);
  uint32_t chan_num = tm_ring_packet_channel_count(cfg_->rd_rsp_port_num);

  for (uint32_t port = 0; port < 2; ++port) {
    auto dir = ring_port_dir(port);
    auto inf = tm_make_com_inf(clk_, name_ + "_port_inf_" +
                                         std::to_string(tm_ring_port_index(dir)),
                               router_input_depth);
    inf->set_chan_num(chan_num);
    port_infs_.push_back(inf);
  }

  auto route_req_proc = TM_MAKE_CPROC(&TmRingRouter::route_request);
  for (auto& inf : port_infs_) {
    tm_sensitive(route_req_proc, inf->vld);
  }

  auto route_rsp_proc = TM_MAKE_CPROC(&TmRingRouter::route_response);
  for (auto& inf : port_infs_) {
    tm_sensitive(route_rsp_proc, inf->vld);
  }

  reset();
}

void TmRingRouter::reset() {
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
  uint32_t rr_init_slot = tm_ring_port_count() * traffic_slot_count() - 1;
  output_rr_ptr_.assign(tm_ring_port_count() * tm_ring_subnet_count(),
                        rr_init_slot);
}

bool TmRingRouter::idle() const {
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
  router_id_ = router_id;
  topology_ = topology;
  east_link_ = east_link;
  west_link_ = west_link;
}

void TmRingRouter::bind_local_master(uint32_t master_port,
                                     p_tm_com_inf_t inf) {
  if (master_port >= local_master_infs_.size()) {
    local_master_infs_.resize(master_port + 1, nullptr);
  }
  if (local_master_infs_[master_port] == nullptr) {
    local_master_infs_[master_port] = tm_make_com_inf(
        clk_, name_ + "_local_master_inf_" + std::to_string(master_port),
        std::max<uint32_t>(1, cfg_->ring_router_input_depth));
    local_master_infs_[master_port]->set_chan_num(
        tm_ring_packet_channel_count(cfg_->rd_rsp_port_num));
    tm_sensitive(TM_MAKE_CPROC(&TmRingRouter::route_request),
                 local_master_infs_[master_port]->vld);
  }
  local_master_infs_[master_port]->connect(inf);
}

void TmRingRouter::bind_local_target(uint32_t target_id,
                                     p_tm_com_inf_t inf) {
  if (target_id >= local_target_infs_.size()) {
    local_target_infs_.resize(target_id + 1, nullptr);
  }
  if (local_target_infs_[target_id] == nullptr) {
    local_target_infs_[target_id] = tm_make_com_inf(
        clk_, name_ + "_local_target_inf_" + std::to_string(target_id),
        std::max<uint32_t>(1, cfg_->ring_router_input_depth));
    local_target_infs_[target_id]->set_chan_num(
        tm_ring_packet_channel_count(cfg_->rd_rsp_port_num));
    tm_sensitive(TM_MAKE_CPROC(&TmRingRouter::route_response),
                 local_target_infs_[target_id]->vld);
  }
  local_target_infs_[target_id]->connect(inf);
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
  if (in_dir != TmRingPortDir::LOCAL) {
    return port_infs_[ring_port_slot(in_dir)];
  }

  uint32_t chan = packet_channel(traffic_class, lane);
  auto cmd = static_cast<PldCmd>(traffic_class);
  if (tm_ring_is_req_cmd(cmd)) {
    for (const auto& inf : local_master_infs_) {
      if (inf != nullptr && inf->valid(chan)) {
        return inf;
      }
    }
    return nullptr;
  }

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

bool TmRingRouter::local_ready(p_tm_pld_t pld) {
  return local_inf(pld) != nullptr;
}

bool TmRingRouter::route_local(p_tm_pld_t pld) {
  auto inf = local_inf(pld);
  if (inf == nullptr) {
    return false;
  }

  if (!inf->send(local_channel(pld), pld)) {
    return false;
  }

  return true;
}

p_tm_com_inf_t TmRingRouter::local_inf(p_tm_pld_t pld) const {
  auto cmd = static_cast<PldCmd>(pld->ring_traffic_class);
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
    if (inf == nullptr) {
      continue;
    }
    uint32_t chan = packet_channel(cls, lane);
    if (!inf->valid(chan)) {
      continue;
    }
    auto pld = inf->get_pld(chan);

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
      auto in_inf = inf_for_class(static_cast<TmRingPortDir>(winner->ring_in_dir),
                                  winner->ring_traffic_class,
                                  winner->ring_rsp_lane);
      in_inf->pop_pld(packet_channel(winner->ring_traffic_class,
                                     winner->ring_rsp_lane));
    }
  }
}
