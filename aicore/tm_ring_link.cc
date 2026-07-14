#include "tm_ring_link.h"

#include <algorithm>
#include <cassert>
#include <iostream>

using namespace tm_engine;

TmRingLink::TmRingLink() {}

TmRingLink::TmRingLink(const std::string& name,
                       p_tm_clk_t clk, p_tm_ring_cfg_t cfg, uint32_t latency,
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
  latency_ = latency;
  width_bytes_ = std::max<uint32_t>(1, cfg_->ring_link_width_bytes);
  dst_router_ = dst_router;
  dst_dir_ = dst_dir;

  ready_packets_.clear();
  next_send_time_.assign(tm_ring_subnet_count(), 0);
  inflight_count_.assign(tm_ring_subnet_count(), 0);
  stats_.assign(tm_ring_subnet_count(), LinkSubnetStats());
  max_inflight_.assign(tm_ring_subnet_count(), 1);
  max_inflight_[tm_ring_subnet_index(TmRingSubnet::REQ)] =
      std::max<uint32_t>(1, cfg_->ring_req_link_max_inflight);
  max_inflight_[tm_ring_subnet_index(TmRingSubnet::RSP)] =
      std::max<uint32_t>(1, cfg_->ring_rsp_link_max_inflight);

  auto drain_proc = TM_MAKE_CPROC(&TmRingLink::drain_ready_packets);
  ready_packets_.push_back(tm_make_que<p_tm_pld_t>(
      clk_, name_ + "_req_ready_packets",
      std::max<uint32_t>(1, cfg_->ring_req_fifo_depth), latency_));
  ready_packets_.push_back(tm_make_que<p_tm_pld_t>(
      clk_, name_ + "_rsp_ready_packets",
      std::max<uint32_t>(1, cfg_->ring_rsp_fifo_depth), latency_));
  for (auto& q : ready_packets_) {
    tm_sensitive(drain_proc, q->vld);
  }

  reset();
}

void TmRingLink::reset() {
  std::fill(next_send_time_.begin(), next_send_time_.end(), 0);
  std::fill(inflight_count_.begin(), inflight_count_.end(), 0);
  std::fill(stats_.begin(), stats_.end(), LinkSubnetStats());
  for (auto& q : ready_packets_) {
    q->clear();
  }
}

bool TmRingLink::idle() const {
  bool ret = true;
  for (auto& q : ready_packets_) {
    ret = ret && q->empty();
  }
  for (auto cnt : inflight_count_) {
    ret = ret && cnt == 0;
  }
  return ret;
}

bool TmRingLink::can_send(p_tm_pld_t pld) const {
  uint32_t idx = pld->ring_subnet;
  return time() >= next_send_time_[idx] && !ready_packets_[idx]->full() &&
         inflight_count_[idx] < max_inflight_[idx];
}

void TmRingLink::enqueue(p_tm_pld_t pld) {
  uint32_t idx = pld->ring_subnet;
  uint32_t bytes = packet_bytes(pld);
  uint32_t serialization_cycles =
      std::max<uint32_t>(1, (bytes + width_bytes_ - 1) / width_bytes_);

  ready_packets_[idx]->push_back(pld);
  inflight_count_[idx]++;
  stats_[idx].packets++;
  stats_[idx].bytes += bytes;
  stats_[idx].busy_cycles += serialization_cycles;
  stats_[idx].inflight_peak =
      std::max(stats_[idx].inflight_peak, inflight_count_[idx]);
  next_send_time_[idx] = time() + serialization_cycles;
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

void TmRingLink::attach(p_tm_com_inf_t req_inf, p_tm_com_inf_t wr_dat_inf,
                        const std::vector<p_tm_com_inf_t>& rd_rsp_infs,
                        p_tm_com_inf_t wr_req_rsp_inf,
                        p_tm_com_inf_t wr_dat_rsp_inf) {
  req_inf_ = req_inf;
  wr_dat_inf_ = wr_dat_inf;
  rd_rsp_infs_ = rd_rsp_infs;
  wr_req_rsp_inf_ = wr_req_rsp_inf;
  wr_dat_rsp_inf_ = wr_dat_rsp_inf;
}

p_tm_com_inf_t TmRingLink::dst_inf(p_tm_pld_t pld) const {
  auto cmd = static_cast<PldCmd>(pld->ring_traffic_class);
  if (cmd == PldCmd::RD || cmd == PldCmd::WR) {
    return req_inf_;
  }
  if (cmd == PldCmd::WR_DAT) {
    return wr_dat_inf_;
  }
  if (cmd == PldCmd::WR_RSP) {
    return wr_req_rsp_inf_;
  }
  if (cmd == PldCmd::RSP) {
    return wr_dat_rsp_inf_;
  }
  if (pld->ring_rsp_lane < rd_rsp_infs_.size()) {
    return rd_rsp_infs_[pld->ring_rsp_lane];
  }
  return nullptr;
}

void TmRingLink::drain_ready_packets() {
  for (uint32_t idx = 0; idx < ready_packets_.size(); ++idx) {
    auto& q = ready_packets_[idx];
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

      auto dst_inf = this->dst_inf(pld);
      if (dst_inf == nullptr) {
        stats_[idx].invalid_dst_stall++;
        std::cerr << name_ << ": missing destination inf for traffic_class "
                  << pld->ring_traffic_class << std::endl;
        assert(false && "TmRingLink missing destination inf");
        break;
      }
      if (!dst_inf->send(pld)) {
        stats_[idx].downstream_inf_full_stall++;
        break;
      }

      if (inflight_count_[idx] > 0) {
        inflight_count_[idx]--;
      }
      q->pop_front();
    }
  }
}

uint32_t TmRingLink::dst_router() const { return dst_router_; }

TmRingPortDir TmRingLink::dst_dir() const { return dst_dir_; }
