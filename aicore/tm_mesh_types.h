#ifndef _TM_MESH_TYPES_H_
#define _TM_MESH_TYPES_H_

#include <stdint.h>

#include <deque>
#include <memory>
#include <string>
#include <vector>

#include "tm_bus_types.h"
#include "tm_mem.h"

enum class tm_mesh_txn_state_t
{
    ALLOCATED,
    IN_INGRESS_FIFO,
    IN_REQ_MESH,
    IN_TARGET_FIFO,
    WAIT_WR_REQ_RSP,
    WAIT_WR_DAT_RSP,
    WAIT_RD_RSP,
    IN_RSP_MESH,
    DONE
};

using tm_mesh_target_cfg_t = tm_bus_target_cfg_t;
using p_tm_mesh_target_cfg_t = p_tm_bus_target_cfg_t;

struct TmMeshCfg
{
    std::string name = "";
    uint32_t num_masters = 1;
    uint32_t num_targets = 1;
    uint32_t rd_rsp_port_num = 2;
    tm_bus_arbiter_type_t arbiter_type = tm_bus_arbiter_type_t::RR;

    uint32_t master_inf_depth = 4;
    uint32_t target_inf_depth = 4;

    uint32_t master_rd_req_fifo_depth = 4;
    uint32_t master_wr_req_fifo_depth = 4;
    uint32_t master_wr_dat_fifo_depth = 8;
    uint32_t master_wr_grant_fifo_depth = 8;
    uint32_t master_wr_req_rsp_fifo_depth = 4;
    uint32_t master_wr_dat_rsp_fifo_depth = 4;
    uint32_t master_rd_rsp_fifo_depth = 8;

    uint32_t mesh_rows = 1;
    uint32_t mesh_cols = 0;
    uint32_t mesh_req_fifo_depth = 4;
    uint32_t mesh_rsp_fifo_depth = 4;
    uint32_t mesh_link_latency = 1;
    bool mesh_x_first = true;

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

struct TmMeshTxnCtx
{
    uint32_t master_port = 0;
    uint32_t target_id = 0;
    uint32_t src_node = 0;
    uint32_t dst_node = 0;
    aic_req_type_t req_type = aic_req_type_t::RD_REQ;
    tm_mesh_txn_state_t state = tm_mesh_txn_state_t::ALLOCATED;
    uint32_t size = 0;
    tm_engine::tm_time_t issue_time = 0;
    uint32_t rsp_expected = 1;
    uint32_t rsp_seen = 0;
    bool slot_released = false;
};

#endif  // _TM_MESH_TYPES_H_
