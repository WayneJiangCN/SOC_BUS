#ifndef _TM_MESH_TYPES_H_
#define _TM_MESH_TYPES_H_

#include <stdint.h>

#include <memory>
#include <string>
#include <vector>

#include "tm_bus_types.h"
#include "tm_mem.h"

using PldCmd = pld_cmd_t;
using plt_cmt_t = PldCmd;
using plt_cmd_t = PldCmd;

using tm_mesh_target_cfg_t = tm_bus_target_cfg_t;
using p_tm_mesh_target_cfg_t = p_tm_bus_target_cfg_t;

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

using TmMeshPortDir = TmRingPortDir;

inline constexpr uint32_t
tm_mesh_port_count()
{
    return tm_ring_port_count();
}

inline constexpr uint32_t
tm_mesh_port_index(TmMeshPortDir dir)
{
    return tm_ring_port_index(dir);
}

inline constexpr TmMeshPortDir
tm_mesh_opposite_dir(TmMeshPortDir dir)
{
    return tm_ring_opposite_dir(dir);
}

struct TmMeshRouteCandidate
{
    bool valid = false;
    TmMeshPortDir in_dir = TmMeshPortDir::LOCAL;
    TmMeshPortDir out_dir = TmMeshPortDir::LOCAL;
    uint32_t traffic_class = 0;
    aic_req_type_t req_type = aic_req_type_t::RD_REQ;
    uint32_t lane = 0;
    p_tm_pld_t pld = nullptr;
};

struct TmMeshCfg
{
    std::string name = "";
    uint32_t num_masters = 1;
    uint32_t num_targets = 1;
    uint32_t rd_rsp_port_num = 2;

    uint32_t master_inf_depth = 4;
    uint32_t target_inf_depth = 4;


    uint32_t master_wr_grant_fifo_depth = 8;

    uint32_t ring_req_fifo_depth = 4;
    uint32_t ring_rsp_fifo_depth = 4;
    uint32_t ring_link_latency = 1;

    bool strict_wr_grant_order = true;

    std::vector<p_tm_mesh_target_cfg_t> targets;
};

using tm_mesh_cfg_t = TmMeshCfg;
using p_tm_mesh_cfg_t = std::shared_ptr<tm_mesh_cfg_t>;

struct TmMeshGrant
{
    uint32_t gid = 0;
    uint32_t target_id = 0;
    uint32_t chan = 0;
    uint32_t dbid = 0;
};

struct TmMeshRdRspState
{
    uint32_t rsp_expected = 1;
    uint32_t rsp_seen = 0;
    bool slot_released = false;
};

#endif  // _TM_MESH_TYPES_H_
