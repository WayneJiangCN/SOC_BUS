#include "tm_ring_link.h"

#include <algorithm>
#include <iostream>

using namespace tm_engine;

namespace {

constexpr uint32_t kRingReqHeaderBytes = 16;
constexpr uint32_t kRingRspHeaderBytes = 16;

}  // namespace

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
  link_capacity_ = std::max<uint32_t>(1, latency_+1);
  // width_bytes_ 至少为 1，避免配置为 0 时出现序列化周期除零。
  width_bytes_ = std::max<uint32_t>(1, cfg_->ring_link_width_bytes);
  dst_router_ = dst_router;
  dst_dir_ = dst_dir;
  PEM_LOG_INFO(log_, "[{0:d}] config latency:{1:d} dst_router:{2:d} "
                     "dst_dir:{3:d} width:{4:d}",
               time(), latency_, dst_router_, tm_ring_port_index(dst_dir_),
               width_bytes_);

  // REQ/RSP 子网独立占用发送资源和 in-flight 配额，彼此不会直接阻塞。
  inflight_packets_.clear();
  const uint32_t lanes = total_lane_count();
  next_send_time_.assign(lanes, 0);
  inflight_count_.assign(lanes, 0);
  lane_stats_.assign(lanes, LinkSubnetStats());
  aggregate_stats_.assign(tm_ring_subnet_count(), LinkSubnetStats());

  // dst_out_inf_ 只负责 Link 到下游 Router 的 valid/ready 交互。
  dst_out_inf_ = tm_make_com_inf(clk_, name_ + "_dst_out_inf",
                                 tm_ring_inf_depth());
  dst_out_inf_->set_chan_num(
      tm_ring_packet_channel_count(cfg_->rd_rsp_port_num));

  // 两个 tm_que 是 Link 的真实在途缓存，delay 参数表达固定传播延迟。
  auto drain_proc = TM_MAKE_CPROC(&TmRingLink::drain_ready_packets);
  for (uint32_t lane = 0; lane < lanes; ++lane) {
    inflight_packets_.push_back(tm_make_que<p_tm_pld_t>(
        clk_, name_ + "_lane" + std::to_string(lane) + "_ready_packets",
        link_capacity_, latency_));
  }
  // 数据达到队列可见时间后触发 drain；无需额外 retry_event_。
  for (auto& q : inflight_packets_) {
    tm_sensitive(drain_proc, q->vld);
  }

  reset();
}

uint32_t TmRingLink::subnet_lane_count(TmRingSubnet subnet) const {
  if (subnet == TmRingSubnet::RSP && cfg_ != nullptr) {
    return tm_ring_rsp_phys_lane_count(cfg_->rsp_phys_lanes);
  }
  return 1;
}

uint32_t TmRingLink::total_lane_count() const {
  return 1 + subnet_lane_count(TmRingSubnet::RSP);
}

uint32_t TmRingLink::lane_index(TmRingSubnet subnet, uint32_t lane) const {
  if (subnet == TmRingSubnet::REQ) {
    return 0;
  }
  return 1 + (lane % subnet_lane_count(TmRingSubnet::RSP));
}

TmRingSubnet TmRingLink::lane_subnet(uint32_t idx) const {
  return idx == 0 ? TmRingSubnet::REQ : TmRingSubnet::RSP;
}

TmRingSubnet TmRingLink::packet_subnet(p_tm_pld_t pld) const {
  return pld != nullptr && pld->ring_subnet == tm_ring_subnet_index(TmRingSubnet::RSP)
             ? TmRingSubnet::RSP
             : TmRingSubnet::REQ;
}

uint32_t TmRingLink::physical_lane(p_tm_pld_t pld) const {
  if (pld == nullptr || packet_subnet(pld) != TmRingSubnet::RSP) {
    return 0;
  }
  return pld->ring_rsp_phys_lane % subnet_lane_count(TmRingSubnet::RSP);
}

TmRingLink::LinkSubnetStats& TmRingLink::aggregate_stats(
    TmRingSubnet subnet) {
  return aggregate_stats_[tm_ring_subnet_index(subnet)];
}

const TmRingLink::LinkSubnetStats& TmRingLink::aggregate_stats(
    TmRingSubnet subnet) const {
  return aggregate_stats_[tm_ring_subnet_index(subnet)];
}

void TmRingLink::record_stall(uint32_t lane_idx, TmRingSubnet subnet,
                              uint64_t LinkSubnetStats::*field) {
  lane_stats_[lane_idx].*field += 1;
  aggregate_stats(subnet).*field += 1;
}

void TmRingLink::record_packet(uint32_t lane_idx, TmRingSubnet subnet,
                               uint32_t bytes,
                               uint32_t serialization_cycles) {
  auto& lane_stats = lane_stats_[lane_idx];
  auto& subnet_stats = aggregate_stats(subnet);
  lane_stats.packets++;
  lane_stats.bytes += bytes;
  lane_stats.busy_cycles += serialization_cycles;
  lane_stats.inflight_peak =
      std::max(lane_stats.inflight_peak, inflight_count_[lane_idx]);

  subnet_stats.packets++;
  subnet_stats.bytes += bytes;
  subnet_stats.busy_cycles += serialization_cycles;
  subnet_stats.inflight_peak =
      std::max(subnet_stats.inflight_peak, inflight_count_[lane_idx]);
}

void TmRingLink::reset() {
  // 传播队列、在途计数和序列化时间必须成组清理，防止资源状态不一致。
  // dst_out_inf_ 已连接到下游 Router 输入端口，下游端口由 Router 自己 reset。
  std::fill(next_send_time_.begin(), next_send_time_.end(), 0);
  std::fill(inflight_count_.begin(), inflight_count_.end(), 0);
  std::fill(lane_stats_.begin(), lane_stats_.end(), LinkSubnetStats());
  std::fill(aggregate_stats_.begin(), aggregate_stats_.end(),
            LinkSubnetStats());
  for (auto& q : inflight_packets_) {
    q->clear();
  }
}

bool TmRingLink::idle() const {
  // 仅队列为空还不够，在途计数也必须归零，便于发现计数泄漏。
  bool ret = true;
  for (auto& q : inflight_packets_) {
    ret = ret && q->empty();
  }
  for (auto cnt : inflight_count_) {
    ret = ret && cnt == 0;
  }
  return ret;
}

bool TmRingLink::can_accept(p_tm_pld_t pld) {
  if (pld == nullptr) {
    return false;
  }
  // Record one primary reason per rejected attempt so the counters can be
  // added without double-counting. Priority follows the packet admission path.
  const TmRingSubnet subnet = packet_subnet(pld);
  const uint32_t idx = lane_index(subnet, physical_lane(pld));
  if (time() < next_send_time_[idx]) {
    record_stall(idx, subnet, &LinkSubnetStats::serialization_busy_stall);
    record_stall(idx, subnet, &LinkSubnetStats::send_reject_stall);
    return false;
  }
  if (inflight_count_[idx] >= link_capacity_) {
    record_stall(idx, subnet, &LinkSubnetStats::inflight_limit_stall);
    record_stall(idx, subnet, &LinkSubnetStats::send_reject_stall);
    return false;
  }
  if (inflight_packets_[idx]->full()) {
    record_stall(idx, subnet, &LinkSubnetStats::link_fifo_full_stall);
    record_stall(idx, subnet, &LinkSubnetStats::send_reject_stall);
    return false;
  }
  return true;
}

bool TmRingLink::can_accept_preserving_bubble(p_tm_pld_t pld) {
  if (!can_accept(pld)) {
    return false;
  }

  const TmRingSubnet subnet = packet_subnet(pld);
  const uint32_t idx = lane_index(subnet, physical_lane(pld));
  if (link_capacity_ <= 1) {
    return true;
  }
  if (inflight_count_[idx] + 1 >= link_capacity_) {
    record_stall(idx, subnet, &LinkSubnetStats::bubble_reserved_stall);
    record_stall(idx, subnet, &LinkSubnetStats::send_reject_stall);
    return false;
  }
  return true;
}

bool TmRingLink::accept_pkt(p_tm_pld_t pld) {
  // can_accept 失败不改变任何状态，Router 会保留输入队头等待后续调度。
  if (!can_accept(pld)) {
    return false;
  }

  reserve_send(pld);
  enqueue_ready_packet(pld);
  return true;
}

bool TmRingLink::accept_pkt_preserving_bubble(p_tm_pld_t pld) {
  if (!can_accept_preserving_bubble(pld)) {
    return false;
  }

  reserve_send(pld);
  enqueue_ready_packet(pld);
  return true;
}

void TmRingLink::reserve_send(p_tm_pld_t pld) {
  const TmRingSubnet subnet = packet_subnet(pld);
  const uint32_t idx = lane_index(subnet, physical_lane(pld));
  uint32_t bytes = packet_bytes(pld);
  // serialization_cycles 表示该 subnet 的发送器被当前 packet 占用多少周期。
  uint32_t serialization_cycles =
      std::max<uint32_t>(1, (bytes + width_bytes_ - 1) / width_bytes_);

  // 传播延迟和序列化占用是两个概念：前者由 tm_que delay 表达，后者由时间戳表达。
  inflight_count_[idx]++;
  record_packet(idx, subnet, bytes, serialization_cycles);
  next_send_time_[idx] = time() + serialization_cycles;
  PEM_LOG_INFO(log_, "[{0:d}] reserve subnet:{1:d} cmd:{2:d} gid:{3:d} "
                     "bytes:{4:d} ser:{5:d} next_send:{6:d} inflight:{7:d}",
               time(), idx, pld->ring_traffic_class, pld->gid, bytes,
               serialization_cycles, next_send_time_[idx],
               inflight_count_[idx]);
}

void TmRingLink::enqueue_ready_packet(p_tm_pld_t pld) {
  // 入队后需等待 latency_ 周期才会触发 drain_ready_packets()。
  const TmRingSubnet subnet = packet_subnet(pld);
  uint32_t idx = lane_index(subnet, physical_lane(pld));
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

  // RD/WR 仅传请求头，读取的数据长度不应占用请求 Link 带宽。
  auto cmd = static_cast<PldCmd>(pld->ring_traffic_class);
  if (cmd == PldCmd::RD || cmd == PldCmd::WR) {
    return kRingReqHeaderBytes;
  }
  // WR_DAT 和 RD_RSP 携带真实数据，序列化字节数采用 payload size。
  if (cmd == PldCmd::WR_DAT) {
    return pld->size;
  }
  // 写 grant 和写完成响应都只按响应头计算。
  if (cmd == PldCmd::WR_RSP || cmd == PldCmd::RSP) {
    return kRingRspHeaderBytes;
  }
  return pld->size;
}

const TmRingLink::LinkSubnetStats& TmRingLink::subnet_stats(
    TmRingSubnet subnet) const {
  return aggregate_stats(subnet);
}

const TmRingLink::LinkSubnetStats& TmRingLink::subnet_lane_stats(
    TmRingSubnet subnet, uint32_t lane) const {
  return lane_stats_[lane_index(subnet, lane)];
}

void TmRingLink::attach(p_tm_com_inf_t dst_inf) {
  // dst_inf 是相邻 Router 与本 Link 方向相反的输入端口。
  dst_out_inf_->connect(dst_inf);
  PEM_LOG_INFO(log_, "[{0:d}] attach_dst dst_router:{1:d} dst_dir:{2:d}",
               time(), dst_router_, tm_ring_port_index(dst_dir_));
}

uint32_t TmRingLink::dst_channel(p_tm_pld_t pld) const {
  return tm_ring_packet_channel(static_cast<PldCmd>(pld->ring_traffic_class),
                                pld->ring_rsp_lane);
}

void TmRingLink::drain_ready_packets() {
  // 每个子网分别搬运已到达的 packet，队头受阻不会影响另一子网。
  for (uint32_t idx = 0; idx < inflight_packets_.size(); ++idx) {
    auto& q = inflight_packets_[idx];
    while (q->valid() && !q->empty()) {
      const TmRingSubnet subnet = lane_subnet(idx);
      auto pld = q->front();
      // nullptr 是模型内部非法项：记录并清理，避免永久卡住整个 Link。
      if (pld == nullptr) {
        record_stall(idx, subnet, &LinkSubnetStats::null_payload_drop);
        if (inflight_count_[idx] > 0) {
          inflight_count_[idx]--;
        }
        std::cerr << name_ << ": null payload in ring link ready queue"
                  << std::endl;
        q->pop_front();
        continue;
      }

      // 下游满时 break 且不 pop，队头继续保持有效并自然形成反压。
      if (!dst_out_inf_->send(dst_channel(pld), pld)) {
        record_stall(idx, subnet,
                     &LinkSubnetStats::downstream_inf_full_stall);
        PEM_LOG_INFO(log_, "[{0:d}] dst_full subnet:{1:d} cmd:{2:d} gid:{3:d}",
                     time(), idx, pld->ring_traffic_class, pld->gid);
        break;
      }

      // 只有下游 Router 成功接收后，Link in-flight 才真正释放。
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
