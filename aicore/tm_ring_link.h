#ifndef _TM_RING_LINK_H_
#define _TM_RING_LINK_H_

#include <stdint.h>

#include <memory>
#include <string>
#include <vector>

#include "pem_log.h"
#include "tm_clock.h"
#include "tm_engine.h"
#include "tm_que.h"
#include "tm_ring_types.h"

class TmRingLink : public tm_engine::TmModule {
 public:
  struct LinkSubnetStats {
    // REQ/RSP 子网分别统计，便于定位请求拥塞或响应拥塞。
    uint64_t packets = 0;
    uint64_t bytes = 0;
    uint64_t busy_cycles = 0;
    uint64_t downstream_inf_full_stall = 0;
    uint64_t serialization_busy_stall = 0;
    uint64_t inflight_limit_stall = 0;
    uint64_t link_fifo_full_stall = 0;
    uint64_t bubble_reserved_stall = 0;
    // Compatibility aggregate of the three can_accept rejection reasons.
    uint64_t send_reject_stall = 0;
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

  bool can_accept(p_tm_pld_t pld);
  bool can_accept_preserving_bubble(p_tm_pld_t pld);
  bool accept_pkt_preserving_bubble(p_tm_pld_t pld);
  // 成功时同时占用序列化带宽和 in-flight 名额，并进入传播延迟队列。
  bool accept_pkt(p_tm_pld_t pld);
  // 根据命令类型区分头包和数据包，避免把 RD 请求误算成完整读数据大小。
  uint32_t packet_bytes(p_tm_pld_t pld) const;
  const LinkSubnetStats& subnet_stats(TmRingSubnet subnet) const;
  void attach(p_tm_com_inf_t dst_inf);
  uint32_t dst_router() const;
  TmRingPortDir dst_dir() const;

 private:
  // reserve_send 记录带宽占用；enqueue_ready_packet 负责进入延迟队列。
  void reserve_send(p_tm_pld_t pld);
  void enqueue_ready_packet(p_tm_pld_t pld);
  // 队列延迟到期后尝试发送给下游 Router；下游阻塞时保留队头。
  void drain_ready_packets();
  uint32_t dst_channel(p_tm_pld_t pld) const;

  std::string name_;
  tm_engine::p_tm_clk_t clk_ = nullptr;
  p_tm_ring_cfg_t cfg_ = nullptr;
  uint32_t latency_ = 1;
  uint32_t link_capacity_ = 1;
  uint32_t width_bytes_ = 16;
  uint32_t dst_router_ = 0;
  TmRingPortDir dst_dir_ = TmRingPortDir::LOCAL;
  // 每个 subnet 独立维护发送时间、在途计数和传播队列。
  std::vector<uint32_t> inflight_count_;
  std::vector<tm_engine::tm_time_t> next_send_time_;
  std::vector<p_tm_com_que_t> inflight_packets_;
  std::vector<LinkSubnetStats> stats_;
  // dst_out_inf_ 是 Link 的发送端，通过 connect() 连接下游 Router 输入端。
  p_tm_com_inf_t dst_out_inf_ = nullptr;
  p_logger_t log_ = nullptr;
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
