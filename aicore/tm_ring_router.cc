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
  log_para_t log_para(name_ + ".log");
  log_ = pem_log::create_logger(log_para);
  PEM_LOG_INFO(log_, "[{0:d}] config", time());

  port_infs_.clear();
  local_master_infs_.clear();
  local_target_infs_.clear();
  uint32_t rr_init_slot = tm_ring_port_count() * traffic_slot_count() - 1;
  output_rr_ptr_.assign(tm_ring_port_count() * tm_ring_subnet_count(),
                        rr_init_slot);
  uint32_t chan_num = tm_ring_packet_channel_count(cfg_->rd_rsp_port_num);

  for (uint32_t port = 0; port < 2; ++port) {
    auto dir = ring_port_dir(port);
    auto inf = tm_make_com_inf(
        clk_, name_ + "_port_inf_" +
                  std::to_string(tm_ring_port_index(dir)),
        cfg_->master_inf_depth);
    inf->set_chan_num(chan_num);
    port_infs_.push_back(inf);
  }

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
  if (master_port >= local_master_infs_.size()) {
    local_master_infs_.resize(master_port + 1, nullptr);
  }
  if (local_master_infs_[master_port] == nullptr) {
    local_master_infs_[master_port] = tm_make_com_inf(
        clk_, name_ + "_local_master_inf_" + std::to_string(master_port),
        cfg_->master_inf_depth);
    local_master_infs_[master_port]->set_chan_num(
        tm_ring_packet_channel_count(cfg_->rd_rsp_port_num));
    tm_sensitive(TM_MAKE_CPROC(&TmRingRouter::route_local_request),
                 local_master_infs_[master_port]->vld);
  }
  local_master_infs_[master_port]->connect(inf);
  PEM_LOG_INFO(log_, "[{0:d}] bind_local_master port:{1:d}",
               time(), master_port);
}

void TmRingRouter::bind_local_target(uint32_t target_id,
                                     p_tm_com_inf_t inf) {
  if (target_id >= local_target_infs_.size()) {
    local_target_infs_.resize(target_id + 1, nullptr);
  }
  if (local_target_infs_[target_id] == nullptr) {
    local_target_infs_[target_id] = tm_make_com_inf(
        clk_, name_ + "_local_target_inf_" + std::to_string(target_id),
        cfg_->target_inf_depth);
    local_target_infs_[target_id]->set_chan_num(
        tm_ring_packet_channel_count(cfg_->rd_rsp_port_num));
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
  uint32_t extra_rd_rsp_lanes =
      cfg_->rd_rsp_port_num > 0 ? cfg_->rd_rsp_port_num - 1 : 0;
  return tm_ring_base_packet_channel_count() + extra_rd_rsp_lanes;
}

void TmRingRouter::decode_slot(uint32_t slot_class, uint32_t& traffic_class,
                               uint32_t& rsp_lane) {
  traffic_class = slot_class;
  rsp_lane = 0;

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

TmRingPortDir TmRingRouter::resolve_route(p_tm_pld_t pld) {
  auto cmd = static_cast<PldCmd>(pld->ring_traffic_class);
  bool is_request =
      cmd == PldCmd::RD || cmd == PldCmd::WR || cmd == PldCmd::WR_DAT;
  uint32_t destination =
      is_request ? tm_pld_dst_node(pld) : tm_pld_src_node(pld);
  return router_id_ == destination
             ? TmRingPortDir::LOCAL
             : topology_->route_direction(router_id_, destination);
}

bool TmRingRouter::route_ready(p_tm_ring_candidate_t candidate) {
  auto out_dir = candidate->out_dir;
  if (out_dir == TmRingPortDir::LOCAL) {
    return local_ready(candidate->pld);
  }

  auto link = output_link(out_dir);
  return link != nullptr && link->can_send(candidate->pld);
}

bool TmRingRouter::route_packet(p_tm_ring_candidate_t candidate) {
  auto pld = candidate->pld;
  auto out_dir = candidate->out_dir;
  if (out_dir == TmRingPortDir::LOCAL) {
    return route_local(pld);
  }

  auto link = output_link(out_dir);
  return link != nullptr && link->send_pkt(pld);
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
p_tm_ring_candidate_t TmRingRouter::select_input_candidate(TmRingPortDir in_dir,
                                                           TmRingSubnet subnet) {
  auto candidate = std::make_shared<tm_ring_candidate_t>();
  uint32_t class_num = traffic_slot_count();
  for (uint32_t slot_class = 0; slot_class < class_num; ++slot_class) {
    uint32_t cls;
    uint32_t lane;
    decode_slot(slot_class, cls, lane);

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

    candidate->pld = pld;
    candidate->in_dir = in_dir;
    candidate->out_dir = resolve_route(pld);
    candidate->slot_id = tm_ring_port_index(in_dir) * class_num + slot_class;

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

  if (route_packet(winner)) {
    commit_packet(subnet, winner);
  }
}

void TmRingRouter::commit_packet(TmRingSubnet subnet,
                                 p_tm_ring_candidate_t candidate) {
  auto pld = candidate->pld;
  uint32_t subnet_idx = tm_ring_subnet_index(subnet);
  uint32_t out_idx =
      subnet_idx * tm_ring_port_count() + tm_ring_port_index(candidate->out_dir);
  output_rr_ptr_[out_idx] = candidate->slot_id;

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
