#ifndef _TM_RING_LINK_H_
#define _TM_RING_LINK_H_

#include <stdint.h>

#include <memory>
#include <string>
#include <vector>

#include "tm_clock.h"
#include "tm_engine.h"
#include "tm_que.h"
#include "tm_ring_types.h"

class TmRingLink : public tm_engine::TmModule {
 public:
  struct LinkSubnetStats {
    uint64_t packets = 0;
    uint64_t bytes = 0;
    uint64_t busy_cycles = 0;
    uint64_t downstream_inf_full_stall = 0;
    uint64_t invalid_dst_stall = 0;
    uint64_t null_payload_drop = 0;
    uint32_t inflight_peak = 0;
  };

  TmRingLink();
  TmRingLink(const std::string& name, tm_engine::p_tm_clk_t clk,
             p_tm_ring_cfg_t cfg, uint32_t latency, uint32_t dst_router,
             TmRingPortDir dst_dir);
  ~TmRingLink();

  void config(const std::string& name, tm_engine::p_tm_clk_t clk,
              p_tm_ring_cfg_t cfg, uint32_t latency, uint32_t dst_router,
              TmRingPortDir dst_dir);
  void reset();
  bool idle() const;

  bool can_send(p_tm_pld_t pld) const;
  void enqueue(p_tm_pld_t pld);
  uint32_t packet_bytes(p_tm_pld_t pld) const;
  const LinkSubnetStats& subnet_stats(TmRingSubnet subnet) const;
  void attach(p_tm_com_inf_t req_inf, p_tm_com_inf_t wr_dat_inf,
              const std::vector<p_tm_com_inf_t>& rd_rsp_infs,
              p_tm_com_inf_t wr_req_rsp_inf, p_tm_com_inf_t wr_dat_rsp_inf);
  uint32_t dst_router() const;
  TmRingPortDir dst_dir() const;

 private:
  void drain_ready_packets();
  p_tm_com_inf_t dst_inf(p_tm_pld_t pld) const;

  std::string name_;
  tm_engine::p_tm_clk_t clk_ = nullptr;
  p_tm_ring_cfg_t cfg_ = nullptr;
  uint32_t latency_ = 1;
  uint32_t width_bytes_ = 16;
  uint32_t dst_router_ = 0;
  TmRingPortDir dst_dir_ = TmRingPortDir::LOCAL;
  std::vector<uint32_t> max_inflight_;
  std::vector<uint32_t> inflight_count_;
  std::vector<tm_engine::tm_time_t> next_send_time_;
  std::vector<p_tm_com_que_t> ready_packets_;
  std::vector<LinkSubnetStats> stats_;
  p_tm_com_inf_t req_inf_ = nullptr;
  p_tm_com_inf_t wr_dat_inf_ = nullptr;
  std::vector<p_tm_com_inf_t> rd_rsp_infs_;
  p_tm_com_inf_t wr_req_rsp_inf_ = nullptr;
  p_tm_com_inf_t wr_dat_rsp_inf_ = nullptr;
};

using tm_ring_link_t = TmRingLink;
using p_tm_ring_link_t = std::shared_ptr<tm_ring_link_t>;

inline p_tm_ring_link_t tm_make_ring_link(const std::string& name,
                                          tm_engine::p_tm_clk_t clk,
                                          p_tm_ring_cfg_t cfg, uint32_t latency,
                                          uint32_t dst_router,
                                          TmRingPortDir dst_dir) {
  return std::make_shared<TmRingLink>(name, clk, cfg, latency, dst_router,
                                      dst_dir);
}

#endif  // _TM_RING_LINK_H_
