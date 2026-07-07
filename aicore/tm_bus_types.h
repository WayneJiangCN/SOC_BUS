#ifndef _TM_BUS_TYPES_H_
#define _TM_BUS_TYPES_H_

#include <stdint.h>

#include <deque>
#include <memory>
#include <string>
#include <vector>

#include "tm_mem.h"

/* ring 版本互连用到的配置、状态与事务上下文统一定义在这里。 */
enum class tm_bus_txn_state_t
{
    ALLOCATED,
    IN_INGRESS_FIFO,
    IN_REQ_RING,
    IN_TARGET_FIFO,
    WAIT_WR_REQ_RSP,
    WAIT_WR_DAT_RSP,
    WAIT_RD_RSP,
    IN_RSP_RING,
    DONE
};

enum class tm_bus_arbiter_type_t
{
    RR,
    ISLIP_LIKE
};

enum class tm_bus_interleave_type_t
{
    LINEAR,
    XOR_HASH
};

struct TmBusTargetCfg
{
    /* 地址范围与基础命名。 */
    std::string name = "";
    uint64_t addr_begin = 0;
    uint64_t size = 0;
    bool is_default = false;

    /* interleave / hash 切片参数。 */
    tm_bus_interleave_type_t interleave_type =
        tm_bus_interleave_type_t::LINEAR;
    uint32_t interleave_size = 0;
    uint32_t interleave_num = 1;
    uint32_t interleave_idx = 0;
    uint32_t interleave_hash_shift = 6;
    uint32_t interleave_hash_seed = 0;

    /* 端点侧时延与宽度模型。 */
    uint32_t frontend_latency = 1;
    uint32_t forward_latency = 0;
    uint32_t response_latency = 1;
    uint32_t header_latency = 1;
    uint32_t width = 32;

    /* target 本地 FIFO 深度。 */
    uint32_t rd_req_fifo_depth = 8;
    uint32_t wr_req_fifo_depth = 8;
    uint32_t wr_dat_fifo_depth = 8;

    /* outstanding 类 credit。 */
    uint32_t rd_slot_credit_max = 4096;
    uint32_t wr_slot_credit_max = 4096;
    uint32_t acc_slot_credit_max = 4096;

    /* 带宽 token。 */
    uint32_t acc_bw_token_max = 4096;
    uint32_t rd_bw_token_max = 4096;
    uint32_t wr_bw_token_max = 4096;
    uint32_t token_update_period = 1;
    uint32_t acc_bw_token_update = 4096;
    uint32_t rd_bw_token_update = 4096;
    uint32_t wr_bw_token_update = 4096;

    /* 粗粒度热点惩罚。 */
    uint32_t hotspot_threshold = 16;
    uint32_t hotspot_penalty = 0;

    bool contains(uint64_t addr) const
    {
        if (size == 0) {
            return false;
        }
        return addr >= addr_begin && addr < (addr_begin + size);
    }

    bool interleave_enabled() const
    {
        return interleave_num > 1 && interleave_size != 0;
    }
};

using tm_bus_target_cfg_t = TmBusTargetCfg;
using p_tm_bus_target_cfg_t = std::shared_ptr<tm_bus_target_cfg_t>;

struct TmBusCfg
{
    /* 全局实例配置。 */
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

    /* ring 内部 request/response FIFO 与链路时延。 */
    uint32_t ring_req_fifo_depth = 4;
    uint32_t ring_rsp_fifo_depth = 4;
    uint32_t ring_link_latency = 1;

    bool strict_wr_grant_order = true;

    std::vector<p_tm_bus_target_cfg_t> targets;
};

using tm_bus_cfg_t = TmBusCfg;
using p_tm_bus_cfg_t = std::shared_ptr<tm_bus_cfg_t>;

struct TmBusGrant
{
    /* gid + target_id 用于约束 WR_DAT 能否继续发送。 */
    uint32_t gid = 0;
    uint32_t target_id = 0;
    uint32_t chan = 0;
    uint32_t dbid = 0;
};

struct TmBusTxnCtx
{
    /* 一笔事务的源、宿、状态与完成计数。 */
    uint32_t master_port = 0;
    uint32_t target_id = 0;
    uint32_t src_node = 0;
    uint32_t dst_node = 0;
    aic_req_type_t req_type = aic_req_type_t::RD_REQ;
    tm_bus_txn_state_t state = tm_bus_txn_state_t::ALLOCATED;
    uint32_t size = 0;
    tm_engine::tm_time_t issue_time = 0;
    uint32_t rsp_expected = 1;
    uint32_t rsp_seen = 0;
    bool slot_released = false;
};

#endif  // _TM_BUS_TYPES_H_
