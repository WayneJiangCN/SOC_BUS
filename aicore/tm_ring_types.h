#ifndef _TM_RING_TYPES_H_
#define _TM_RING_TYPES_H_

#include <stdint.h>

#include <memory>
#include <string>
#include <vector>

#include "tm_bus_types.h"
#include "tm_mem.h"

using PldCmd = pld_cmd_t;
using plt_cmt_t = PldCmd;
using plt_cmd_t = PldCmd;

using tm_ring_target_cfg_t = tm_bus_target_cfg_t;
using p_tm_ring_target_cfg_t = p_tm_bus_target_cfg_t;

enum class TmRingPortDir : uint32_t {
  // LOCAL 表示当前 Router 挂接的 Master NIU 或 TargetPort。
  LOCAL = 0,
  // EAST 表示顺时针方向。
  EAST = 1,
  // WEST 表示逆时针方向。
  WEST = 2,
};

enum class TmRingSubnet : uint32_t {
  // 请求和响应使用独立子网，避免响应被请求流量长期阻塞。
  REQ = 0,
  RSP = 1,
};

enum class TmRingRspLaneSelect : uint32_t {
  TARGET = 0,
  HASH = 1,
  ROUND_ROBIN = 2,
};

inline constexpr uint32_t tm_ring_port_count() { return 3; }

inline constexpr uint32_t tm_ring_subnet_count() { return 2; }

inline constexpr uint32_t tm_ring_rsp_phys_lane_count(uint32_t lanes) {
  return lanes == 0 ? 1 : lanes;
}

// TmInf 只作为模块间 valid/ready 事件边界，真实缓存由 tm_que 表达。
inline constexpr uint32_t tm_ring_inf_depth() { return 2; }

inline constexpr uint32_t tm_ring_port_index(TmRingPortDir dir) {
  return static_cast<uint32_t>(dir);
}

inline constexpr uint32_t tm_ring_subnet_index(TmRingSubnet subnet) {
  return static_cast<uint32_t>(subnet);
}

inline constexpr bool tm_ring_is_req_cmd(PldCmd cmd) {
  // WR_DAT 是写事务第二阶段，但在 Ring 中仍属于请求方向。
  return cmd == PldCmd::RD || cmd == PldCmd::WR || cmd == PldCmd::WR_DAT;
}

inline constexpr bool tm_ring_is_xor_hash_interleave(
    tm_bus_interleave_type_t type) {
  return type == tm_bus_interleave_type_t::XOR_HASH;
}

inline constexpr aic_req_type_t tm_ring_cmd_to_req(PldCmd cmd) {
  return cmd == PldCmd::RD   ? aic_req_type_t::RD_REQ
         : cmd == PldCmd::WR ? aic_req_type_t::WR_REQ
                             : aic_req_type_t::WR_DAT;
}

inline constexpr uint32_t tm_ring_cmd_bus_channel(PldCmd cmd) {
  return static_cast<uint32_t>(tm_ring_cmd_to_req(cmd));
}

inline constexpr uint32_t tm_ring_req_bus_channel_count() {
  return tm_ring_cmd_bus_channel(PldCmd::RD) + 1;
}

inline constexpr uint32_t tm_ring_rd_rsp_bus_channel(uint32_t lane) {
  return static_cast<uint32_t>(aic_req_type_t::RD_REQ) + lane;
}

inline constexpr uint32_t tm_ring_rsp_bus_channel(PldCmd cmd,
                                                  uint32_t lane = 0) {
  // 读响应按 lane 分流；写命令响应和写数据响应复用对应请求通道编号。
  return cmd == PldCmd::RD_RSP   ? tm_ring_rd_rsp_bus_channel(lane)
         : cmd == PldCmd::WR_RSP ? tm_ring_cmd_bus_channel(PldCmd::WR)
                                 : tm_ring_cmd_bus_channel(PldCmd::WR_DAT);
}

inline constexpr uint32_t tm_ring_rsp_bus_channel_count(
    uint32_t rd_rsp_port_num) {
  return tm_ring_rd_rsp_bus_channel(0) + rd_rsp_port_num;
}

inline constexpr uint32_t tm_ring_base_packet_channel_count() {
  /*
   * 基础通道覆盖 RD、WR、WR_DAT、RSP、RD_RSP(lane0) 和 WR_RSP。
   * 额外 RD_RSP lane 依次追加在基础命令通道之后。
   */
  return static_cast<uint32_t>(PldCmd::WR_RSP) + 1;
}

inline constexpr uint32_t tm_ring_packet_channel(PldCmd cmd,
                                                 uint32_t lane = 0) {
  // lane0 直接使用 RD_RSP 枚举值，额外 lane 追加到基础命令通道之后。
  return cmd == PldCmd::RD_RSP
             ? (lane == 0 ? static_cast<uint32_t>(PldCmd::RD_RSP)
                          : tm_ring_base_packet_channel_count() + lane - 1)
             : static_cast<uint32_t>(cmd);
}

inline constexpr uint32_t tm_ring_packet_channel_count(
    uint32_t rd_rsp_port_num) {
  return tm_ring_base_packet_channel_count() + rd_rsp_port_num - 1;
}

inline constexpr TmRingPortDir tm_ring_opposite_dir(TmRingPortDir dir) {
  switch (dir) {
    case TmRingPortDir::EAST:
      return TmRingPortDir::WEST;
    case TmRingPortDir::WEST:
      return TmRingPortDir::EAST;
    case TmRingPortDir::LOCAL:
    default:
      return TmRingPortDir::LOCAL;
  }
}

struct TmRingCfg {
  // Ring 实例名称，用于模块命名和日志文件前缀。
  std::string name = "";
  // Master/NIU 数量。
  uint32_t num_masters = 1;
  // 读响应返回通道数量；lane0 使用 RD_RSP 基础通道。
  uint32_t rd_rsp_port_num = 2;
  // RSP 物理 lane 数；rd_rsp_port_num 仅表示逻辑响应通道数。
  uint32_t rsp_phys_lanes = 1;
  TmRingRspLaneSelect rsp_lane_select = TmRingRspLaneSelect::TARGET;

  // Master NIU 内部 FIFO 深度，是真正的 master 侧缓存资源。
  uint32_t master_rd_cmd_fifo_depth = 8;
  uint32_t master_wr_cmd_fifo_depth = 8;
  uint32_t master_wr_dat_fifo_depth = 8;
  uint32_t master_wr_rsp_fifo_depth = 8;

  // 单个 master 允许的读/写事务 outstanding 上限。
  uint32_t master_rd_osd = 8;
  uint32_t master_wr_osd = 8;
  // 全局事务 outstanding 上限。
  uint32_t global_osd = 128;

  // Ring 每跳固定传播延迟，由 Link 内部 tm_que delay 表达。
  uint32_t ring_link_latency = 1;
  // Link 每周期可序列化发送的字节数。
  uint32_t ring_link_width_bytes = 16;
  // Router EAST/WEST 输入缓存深度，用于让到站 packet 先离开 Link。
  uint32_t ring_router_input_depth = 2;
  // Target 配置列表；运行时 Target 数量以 targets.size() 为准。
  std::vector<p_tm_ring_target_cfg_t> targets;
};

using tm_ring_cfg_t = TmRingCfg;
using p_tm_ring_cfg_t = std::shared_ptr<tm_ring_cfg_t>;

struct TmRingRdRspState {
  // 一笔读事务可能返回多个响应分片，全部到齐后才算完成。
  uint32_t rsp_expected = 1;
  uint32_t rsp_seen = 0;
  // Target credit 只能释放一次，防止多分片响应重复释放。
  bool slot_released = false;
};

#endif  // _TM_RING_TYPES_H_
