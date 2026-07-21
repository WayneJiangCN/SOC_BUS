#ifndef _TM_BUS_TYPES_H_
#define _TM_BUS_TYPES_H_

#include <stdint.h>

#include <deque>
#include <memory>
#include <string>
#include <vector>

#include "tm_mem.h"

enum class tm_bus_interleave_type_t
{
    LINEAR,
    XOR_HASH
};

struct TmBusTargetCfg
{
    std::string name = "";
    uint64_t addr_begin = 0;
    uint64_t size = 0;
    bool is_default = false;

    tm_bus_interleave_type_t interleave_type =
        tm_bus_interleave_type_t::LINEAR;
    uint32_t interleave_size = 0;
    uint32_t interleave_num = 1;
    uint32_t interleave_idx = 0;
    uint32_t interleave_hash_shift = 6;
    uint32_t interleave_hash_seed = 0;

    uint32_t frontend_latency = 1;
    uint32_t forward_latency = 0;
    uint32_t response_latency = 1;
    uint32_t header_latency = 1;
    uint32_t width = 32;

    uint32_t rd_req_fifo_depth = 8;
    uint32_t wr_req_fifo_depth = 8;
    uint32_t wr_dat_fifo_depth = 8;

    uint32_t rd_slot_credit_max = 4096;
    uint32_t wr_slot_credit_max = 4096;
    uint32_t acc_slot_credit_max = 4096;

    uint32_t acc_bw_token_max = 4096;
    uint32_t rd_bw_token_max = 4096;
    uint32_t wr_bw_token_max = 4096;
    uint32_t token_update_period = 1;
    uint32_t acc_bw_token_update = 4096;
    uint32_t rd_bw_token_update = 4096;
    uint32_t wr_bw_token_update = 4096;

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
    std::string name = "";
    uint32_t num_masters = 1;
    uint32_t num_targets = 1;
    uint32_t rd_rsp_port_num = 2;

    // TmInf capacity is derived as delay + 1 when the endpoint is created.
    uint32_t master_inf_delay = 3;
    uint32_t target_inf_delay = 3;

    uint32_t master_rd_req_fifo_depth = 4;
    uint32_t master_wr_req_fifo_depth = 4;
    uint32_t master_wr_dat_fifo_depth = 8;
    uint32_t master_wr_grant_fifo_depth = 8;

    uint32_t ring_link_latency = 1;

    bool strict_wr_grant_order = true;
    uint32_t global_osd = 128;

    std::vector<p_tm_bus_target_cfg_t> targets;
};

using tm_bus_cfg_t = TmBusCfg;
using p_tm_bus_cfg_t = std::shared_ptr<tm_bus_cfg_t>;

#endif  // _TM_BUS_TYPES_H_
