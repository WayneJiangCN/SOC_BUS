#include "tm_mesh_link.h"

#include <algorithm>
#include <cassert>
#include <iostream>
#include <utility>

#include "tm_mesh_router.h"

using namespace tm_engine;

TmMeshLink::TmMeshLink() {}

TmMeshLink::TmMeshLink(const std::string& name,
                       p_tm_clk_t clk, p_tm_mesh_cfg_t cfg, uint32_t latency,
                       uint32_t dst_router, TmMeshPortDir dst_dir)
    : TmModule(name) {
  config(name, clk, cfg, latency, dst_router, dst_dir);
}

TmMeshLink::~TmMeshLink() {}

void TmMeshLink::config(const std::string& name, p_tm_clk_t clk,
                        p_tm_mesh_cfg_t cfg, uint32_t latency,
                        uint32_t dst_router, TmMeshPortDir dst_dir) {
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

  auto drain_proc = TM_MAKE_CPROC(&TmMeshLink::drain_ready_packets);
  ready_packets_.push_back(tm_make_que<Transit>(
      clk_, name_ + "_req_ready_packets",
      std::max<uint32_t>(1, cfg_->ring_req_fifo_depth), latency_));
  ready_packets_.push_back(tm_make_que<Transit>(
      clk_, name_ + "_rsp_ready_packets",
      std::max<uint32_t>(1, cfg_->ring_rsp_fifo_depth), latency_));
  for (auto& q : ready_packets_) {
    tm_sensitive(drain_proc, q->vld);
  }

  reset();
}

void TmMeshLink::reset() {
  std::fill(next_send_time_.begin(), next_send_time_.end(), 0);
  std::fill(inflight_count_.begin(), inflight_count_.end(), 0);
  std::fill(stats_.begin(), stats_.end(), LinkSubnetStats());
  for (auto& q : ready_packets_) {
    q->clear();
  }
}

bool TmMeshLink::idle() const {
  bool ret = true;
  for (auto& q : ready_packets_) {
    ret = q->empty();
  }
  for (auto cnt : inflight_count_) {
    ret = cnt == 0;
  }
  return ret;
}

bool TmMeshLink::can_send(TmRingSubnet subnet, tm_time_t now) const {
  uint32_t idx = tm_ring_subnet_index(subnet);
  return now >= next_send_time_[idx] && !ready_packets_[idx]->full() &&
         inflight_count_[idx] < max_inflight_[idx];
}

void TmMeshLink::enqueue(TmRingSubnet subnet, p_tm_pld_t pld,
                         uint32_t traffic_class, tm_time_t now) {
  uint32_t idx = tm_ring_subnet_index(subnet);
  uint32_t bytes = packet_bytes(traffic_class, pld);
  uint32_t serialization_cycles =
      std::max<uint32_t>(1, (bytes + width_bytes_ - 1) / width_bytes_);

  Transit transit;
  transit.pld = pld;
  transit.traffic_class = traffic_class;
  transit.packet_bytes = bytes;
  transit.serialization_cycles = serialization_cycles;
  transit.tx_start_time = now;

  ready_packets_[idx]->push_back(transit);
  inflight_count_[idx]++;
  stats_[idx].packets++;
  stats_[idx].bytes += bytes;
  stats_[idx].busy_cycles += serialization_cycles;
  stats_[idx].inflight_peak =
      std::max(stats_[idx].inflight_peak, inflight_count_[idx]);
  next_send_time_[idx] = now + serialization_cycles;
}

uint32_t TmMeshLink::packet_bytes(uint32_t traffic_class,
                                  const p_tm_pld_t& pld) const {
  if (pld == nullptr) {
    return 0;
  }

  auto cmd = static_cast<PldCmd>(traffic_class);
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

const TmMeshLink::LinkSubnetStats& TmMeshLink::subnet_stats(
    TmRingSubnet subnet) const {
  return stats_[tm_ring_subnet_index(subnet)];
}

void TmMeshLink::attach(dst_fifo_lookup_t dst_fifo_lookup) {
  dst_fifo_lookup_ = std::move(dst_fifo_lookup);
}

void TmMeshLink::drain_ready_packets() {
  for (uint32_t idx = 0; idx < ready_packets_.size(); ++idx) {
    auto& q = ready_packets_[idx];
    while (q->valid() && !q->empty()) {
      auto transit = q->front();
      if (transit.pld == nullptr) {
        stats_[idx].null_payload_drop++;
        if (inflight_count_[idx] > 0) {
          inflight_count_[idx]--;
        }
        std::cerr << name_ << ": null payload in ring link transit"
                  << std::endl;
        q->pop_front();
        continue;
      }

      auto dst_fifo = dst_fifo_lookup_(dst_router_, dst_dir_,
                                       transit.traffic_class, transit.pld);
      if (dst_fifo == nullptr) {
        stats_[idx].invalid_dst_stall++;
        std::cerr << name_ << ": missing destination FIFO for traffic_class "
                  << transit.traffic_class << std::endl;
        assert(false && "TmMeshLink missing destination FIFO");
        break;
      }
      if (dst_fifo->full()) {
        stats_[idx].downstream_fifo_full_stall++;
        break;
      }

      dst_fifo->push_back(transit.pld);
      if (inflight_count_[idx] > 0) {
        inflight_count_[idx]--;
      }
      q->pop_front();
    }
  }
}

uint32_t TmMeshLink::dst_router() const { return dst_router_; }

TmMeshPortDir TmMeshLink::dst_dir() const { return dst_dir_; }
