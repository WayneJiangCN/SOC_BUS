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
  LOCAL = 0,
  EAST = 1,
  WEST = 2,
};

enum class TmRingSubnet : uint32_t {
  REQ = 0,
  RSP = 1,
};

inline constexpr uint32_t tm_ring_port_count() { return 3; }

inline constexpr uint32_t tm_ring_subnet_count() { return 2; }

inline constexpr uint32_t tm_ring_port_index(TmRingPortDir dir) {
  return static_cast<uint32_t>(dir);
}

inline constexpr uint32_t tm_ring_subnet_index(TmRingSubnet subnet) {
  return static_cast<uint32_t>(subnet);
}

inline constexpr bool tm_ring_is_req_cmd(PldCmd cmd) {
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
   * Base channels cover:
   * RD, WR, WR_DAT, RSP, RD_RSP(lane0), WR_RSP.
   * Extra RD_RSP lanes are appended after the base command channels.
   */
  return static_cast<uint32_t>(PldCmd::WR_RSP) + 1;
}

inline constexpr uint32_t tm_ring_packet_channel(PldCmd cmd,
                                                 uint32_t lane = 0) {
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
  // Ring 实例名，用于模块命名和日志文件前缀。
  std::string name = "";
  // Master/NIU 数量。
  uint32_t num_masters = 1;
  // Target/Memory partition 数量。
  uint32_t num_targets = 1;
  // 读响应返回通道数量；lane0 使用 RD_RSP 基础通道，额外 lane 追加在后面。
  uint32_t rd_rsp_port_num = 2;

  // Master 侧 TmInf 端口深度。TmInf 只表达握手/端口延迟，不作为长期存储。
  // tm_make_com_inf 默认 delay=1，因此建议 depth=2。
  uint32_t master_inf_depth = 2;
  // Target 侧 TmInf 端口深度，语义同 master_inf_depth。
  uint32_t target_inf_depth = 2;
  // Router 本地/链路端口 TmInf 深度，语义同 master_inf_depth。
  uint32_t ring_router_input_depth = 2;
  // Link 输入/输出 TmInf 深度，语义同 master_inf_depth。
  uint32_t ring_link_inf_depth = 2;

  // Master NIU 内部读命令 FIFO 深度，是真正的 master 侧缓存资源。
  uint32_t master_rd_cmd_fifo_depth = 8;
  // Master NIU 内部写命令 FIFO 深度，是真正的 master 侧缓存资源。
  uint32_t master_wr_cmd_fifo_depth = 8;
  // Master NIU 内部写数据 FIFO 深度，保存 grant 后待注入 Ring 的 WR_DAT。
  uint32_t master_wr_dat_fifo_depth = 8;
  // Master NIU 内部写完成响应 FIFO 深度，保存待返回 BIU 的 WR_DAT_RSP。
  uint32_t master_wr_rsp_fifo_depth = 8;

  // 单个 master 允许的读事务 outstanding 上限。
  uint32_t master_rd_osd = 8;
  // 单个 master 允许的写事务 outstanding 上限。
  uint32_t master_wr_osd = 8;
  // 全局事务 outstanding 上限，当前保留给后续全局 OSD 统计/限制使用。
  uint32_t global_osd = 128;

  // Link 内部 request subnet in-flight FIFO 深度，是真正链路缓存资源。
  uint32_t ring_req_fifo_depth = 4;
  // Link 内部 response subnet in-flight FIFO 深度，是真正链路缓存资源。
  uint32_t ring_rsp_fifo_depth = 4;
  // Ring 每跳固定传播延迟，由 Link 内部 tm_que delay 表达。
  uint32_t ring_link_latency = 1;
  // Link 每周期可序列化发送的字节数。
  uint32_t ring_link_width_bytes = 16;
  // RD/WR 请求头大小，用于计算请求包序列化周期。
  uint32_t ring_req_header_bytes = 16;
  // WR_RSP/RSP 响应头大小，用于计算响应包序列化周期。
  uint32_t ring_rsp_header_bytes = 16;
  // Request subnet 每条 Link 最大在途 packet 数。
  uint32_t ring_req_link_max_inflight = 8;
  // Response subnet 每条 Link 最大在途 packet 数。
  uint32_t ring_rsp_link_max_inflight = 8;
  // Target 配置，包括地址空间、interleave、credit、token 和 target 本地 FIFO。
  std::vector<p_tm_ring_target_cfg_t> targets;
};

using tm_ring_cfg_t = TmRingCfg;
using p_tm_ring_cfg_t = std::shared_ptr<tm_ring_cfg_t>;

struct TmRingRdRspState {
  uint32_t rsp_expected = 1;
  uint32_t rsp_seen = 0;
  bool slot_released = false;
};

#endif  // _TM_RING_TYPES_H_
