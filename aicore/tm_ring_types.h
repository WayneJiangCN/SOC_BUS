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

enum class TmRingPortDir : uint32_t
{
    LOCAL = 0,
    EAST = 1,
    WEST = 2,
};

enum class TmRingSubnet : uint32_t
{
    REQ = 0,
    RSP = 1,
};

inline constexpr uint32_t
tm_ring_port_count()
{
    return 3;
}

inline constexpr uint32_t
tm_ring_subnet_count()
{
    return 2;
}

inline constexpr uint32_t
tm_ring_port_index(TmRingPortDir dir)
{
    return static_cast<uint32_t>(dir);
}

inline constexpr uint32_t
tm_ring_subnet_index(TmRingSubnet subnet)
{
    return static_cast<uint32_t>(subnet);
}

inline constexpr TmRingPortDir
tm_ring_opposite_dir(TmRingPortDir dir)
{
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

struct TmRingCfg
{
    std::string name = "";
    uint32_t num_masters = 1;
    uint32_t num_targets = 1;
    uint32_t rd_rsp_port_num = 2;

    uint32_t master_inf_depth = 4;
    uint32_t target_inf_depth = 4;


    uint32_t master_wr_grant_fifo_depth = 8;
    uint32_t master_rd_osd = 8;
    uint32_t master_wr_osd = 8;
    uint32_t global_osd = 128;

    uint32_t ring_req_fifo_depth = 4;
    uint32_t ring_rsp_fifo_depth = 4;
    uint32_t ring_link_latency = 1;
    uint32_t ring_link_width_bytes = 16;
    uint32_t ring_req_header_bytes = 16;
    uint32_t ring_rsp_header_bytes = 16;
    uint32_t ring_req_link_max_inflight = 8;
    uint32_t ring_rsp_link_max_inflight = 8;

    bool strict_wr_grant_order = true;

    std::vector<p_tm_ring_target_cfg_t> targets;
};

using tm_ring_cfg_t = TmRingCfg;
using p_tm_ring_cfg_t = std::shared_ptr<tm_ring_cfg_t>;

class TmRingLocalEndpoint
{
  public:
    virtual ~TmRingLocalEndpoint() = default;
    virtual bool can_accept_local(p_tm_pld_t pld) = 0;
    virtual bool accept_local(p_tm_pld_t pld) = 0;
};

struct TmRingRdRspState
{
    uint32_t rsp_expected = 1;
    uint32_t rsp_seen = 0;
    bool slot_released = false;
};

#endif  // _TM_RING_TYPES_H_
