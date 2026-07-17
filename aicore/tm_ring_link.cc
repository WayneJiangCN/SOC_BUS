#include "tm_ring_link.h"

#include <algorithm>
#include <cassert>
#include <iostream>

using namespace tm_engine;

TmRingLink::TmRingLink() {}

TmRingLink::TmRingLink(const std::string& name, p_tm_clk_t clk,
                       p_tm_ring_cfg_t cfg, uint32_t latency,
                       uint32_t dst_router, TmRingPortDir dst_dir)
    : TmModule(name) {
  config(name, clk, cfg, latency, dst_router, dst_dir);
}

TmRingLink::~TmRingLink() {}

void TmRingLink::config(const std::string& name, p_tm_clk_t clk,
                        p_tm_ring_cfg_t cfg, uint32_t latency,
                        uint32_t dst_router, TmRingPortDir dst_dir) {
  name_ = name;
  this->name(name_);
  clk_ = clk;
  cfg_ = cfg;
  log_para_t log_para(name_ + ".log");
  log_ = pem_log::create_logger(log_para);
  latency_ = latency;
  width_bytes_ = std::max<uint32_t>(1, cfg_->ring_link_width_bytes);
  dst_router_ = dst_router;
  dst_dir_ = dst_dir;
  PEM_LOG_INFO(log_, "[{0:d}] config latency:{1:d} dst_router:{2:d} "
                     "dst_dir:{3:d} width:{4:d}",
               time(), latency_, dst_router_, tm_ring_port_index(dst_dir_),
               width_bytes_);

  inflight_packets_.clear();
  next_send_time_.assign(tm_ring_subnet_count(), 0);
  inflight_count_.assign(tm_ring_subnet_count(), 0);
  stats_.assign(tm_ring_subnet_count(), LinkSubnetStats());
  max_inflight_.assign(tm_ring_subnet_count(), 1);
  max_inflight_[tm_ring_subnet_index(TmRingSubnet::REQ)] =
      std::max<uint32_t>(1, cfg_->ring_req_link_max_inflight);
  max_inflight_[tm_ring_subnet_index(TmRingSubnet::RSP)] =
      std::max<uint32_t>(1, cfg_->ring_rsp_link_max_inflight);

  dst_out_inf_ = tm_make_com_inf(clk_, name_ + "_dst_out_inf",
                                 cfg_->target_inf_delay + 1);
  dst_out_inf_->set_chan_num(
      tm_ring_packet_channel_count(cfg_->rd_rsp_port_num));

  auto drain_proc = TM_MAKE_CPROC(&TmRingLink::drain_ready_packets);
  inflight_packets_.push_back(tm_make_que<p_tm_pld_t>(
      clk_, name_ + "_req_ready_packets",
      std::max<uint32_t>(1, cfg_->ring_req_fifo_depth), latency_));
  inflight_packets_.push_back(tm_make_que<p_tm_pld_t>(
      clk_, name_ + "_rsp_ready_packets",
      std::max<uint32_t>(1, cfg_->ring_rsp_fifo_depth), latency_));
  for (auto& q : inflight_packets_) {
    tm_sensitive(drain_proc, q->vld);
  }

  reset();
}

void TmRingLink::reset() {
  dst_out_inf_->reset();
  std::fill(next_send_time_.begin(), next_send_time_.end(), 0);
  std::fill(inflight_count_.begin(), inflight_count_.end(), 0);
  std::fill(stats_.begin(), stats_.end(), LinkSubnetStats());
  for (auto& q : inflight_packets_) {
    q->clear();
  }
}

bool TmRingLink::idle() const {
  bool ret = dst_out_inf_->idle();
  for (auto& q : inflight_packets_) {
    ret = ret && q->empty();
  }
  for (auto cnt : inflight_count_) {
    ret = ret && cnt == 0;
  }
  return ret;
}

bool TmRingLink::can_send(p_tm_pld_t pld) {
  if (pld == nullptr) {
    return false;
  }
  uint32_t idx = pld->ring_subnet;
  const bool ready = time() >= next_send_time_[idx] &&
                     !inflight_packets_[idx]->full() &&
                     inflight_count_[idx] < max_inflight_[idx];
  if (!ready) {
    stats_[idx].send_reject_stall++;
  }
  return ready;
}

bool TmRingLink::send_pkt(p_tm_pld_t pld) {
  if (!can_send(pld)) {
    return false;
  }

  reserve_send(pld);
  enqueue_ready_packet(pld);
  return true;
}

void TmRingLink::reserve_send(p_tm_pld_t pld) {
  uint32_t idx = pld->ring_subnet;
  uint32_t bytes = packet_bytes(pld);
  uint32_t serialization_cycles =
      std::max<uint32_t>(1, (bytes + width_bytes_ - 1) / width_bytes_);

  inflight_count_[idx]++;
  stats_[idx].packets++;
  stats_[idx].bytes += bytes;
  stats_[idx].busy_cycles += serialization_cycles;
  stats_[idx].inflight_peak =
      std::max(stats_[idx].inflight_peak, inflight_count_[idx]);
  next_send_time_[idx] = time() + serialization_cycles;
  PEM_LOG_INFO(log_, "[{0:d}] reserve subnet:{1:d} cmd:{2:d} gid:{3:d} "
                     "bytes:{4:d} ser:{5:d} next_send:{6:d} inflight:{7:d}",
               time(), idx, pld->ring_traffic_class, pld->gid, bytes,
               serialization_cycles, next_send_time_[idx],
               inflight_count_[idx]);
}

void TmRingLink::enqueue_ready_packet(p_tm_pld_t pld) {
  uint32_t idx = pld->ring_subnet;
  inflight_packets_[idx]->push_back(pld);
  PEM_LOG_INFO(log_, "[{0:d}] enqueue subnet:{1:d} cmd:{2:d} gid:{3:d} "
                     "inflight:{4:d}",
               time(), idx, pld->ring_traffic_class, pld->gid,
               inflight_count_[idx]);
}

uint32_t TmRingLink::packet_bytes(p_tm_pld_t pld) const {
  if (pld == nullptr) {
    return 0;
  }

  auto cmd = static_cast<PldCmd>(pld->ring_traffic_class);
  if (cmd == PldCmd::RD || cmd == PldCmd::WR) {
    return cfg_->ring_req_header_bytes;
  }
  if (cmd == PldCmd::WR_DAT) {
    return pld->size;
  }
  if (cmd == PldCmd::WR_RSP || cmd == PldCmd::RSP) {
    return cfg_->ring_rsp_header_bytes;
  }
  return pld->size;
}

const TmRingLink::LinkSubnetStats& TmRingLink::subnet_stats(
    TmRingSubnet subnet) const {
  return stats_[tm_ring_subnet_index(subnet)];
}

void TmRingLink::attach(p_tm_com_inf_t dst_inf) {
  dst_inf_ = dst_inf;
  dst_out_inf_->connect(dst_inf_);
  PEM_LOG_INFO(log_, "[{0:d}] attach_dst dst_router:{1:d} dst_dir:{2:d}",
               time(), dst_router_, tm_ring_port_index(dst_dir_));
}

uint32_t TmRingLink::dst_channel(p_tm_pld_t pld) const {
  return tm_ring_packet_channel(static_cast<PldCmd>(pld->ring_traffic_class),
                                pld->ring_rsp_lane);
}

void TmRingLink::drain_ready_packets() {
  for (uint32_t idx = 0; idx < inflight_packets_.size(); ++idx) {
    auto& q = inflight_packets_[idx];
    while (q->valid() && !q->empty()) {
      auto pld = q->front();
      if (pld == nullptr) {
        stats_[idx].null_payload_drop++;
        if (inflight_count_[idx] > 0) {
          inflight_count_[idx]--;
        }
        std::cerr << name_ << ": null payload in ring link ready queue"
                  << std::endl;
        q->pop_front();
        continue;
      }

      if (dst_inf_ == nullptr) {
        stats_[idx].invalid_dst_stall++;
        std::cerr << name_ << ": missing destination inf for traffic_class "
                  << pld->ring_traffic_class << std::endl;
        assert(false && "TmRingLink missing destination inf");
        break;
      }
      if (!dst_out_inf_->send(dst_channel(pld), pld)) {
        stats_[idx].downstream_inf_full_stall++;
        PEM_LOG_INFO(log_, "[{0:d}] dst_full subnet:{1:d} cmd:{2:d} gid:{3:d}",
                     time(), idx, pld->ring_traffic_class, pld->gid);
        break;
      }

      if (inflight_count_[idx] > 0) {
        inflight_count_[idx]--;
      }
      q->pop_front();
      PEM_LOG_INFO(log_, "[{0:d}] drain subnet:{1:d} cmd:{2:d} gid:{3:d} "
                         "dst_router:{4:d} remain:{5:d}",
                   time(), idx, pld->ring_traffic_class, pld->gid,
                   dst_router_, inflight_count_[idx]);
    }
  }
}

uint32_t TmRingLink::dst_router() const { return dst_router_; }

TmRingPortDir TmRingLink::dst_dir() const { return dst_dir_; }
