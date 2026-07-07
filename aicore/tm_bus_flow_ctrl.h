#ifndef _TM_BUS_FLOW_CTRL_H_
#define _TM_BUS_FLOW_CTRL_H_

#include <stdint.h>

#include <vector>

#include "tm_bus_types.h"

/*
 * TmBusFlowCtrl:
 * target 级事务流控模块。
 *
 * 它不关心 ring/mesh 如何逐跳前进，只负责 target 是否还能接受新事务，
 * 以及 slot/token 的消耗与释放。
 */
class TmBusFlowCtrl
{
  public:
    void config(p_tm_bus_cfg_t cfg);
    void reset();
    void update_tokens(tm_engine::tm_time_t now);

    bool can_send_to_target(uint32_t target_id, aic_req_type_t req_type,
                            p_tm_pld_t pld) const;
    bool wr_grant_match(uint32_t grant_target_id, uint32_t grant_gid,
                        uint32_t target_id, p_tm_pld_t pld,
                        bool strict_order) const;

    void consume_target_credit(uint32_t target_id, aic_req_type_t req_type,
                               p_tm_pld_t pld);
    void release_target_credit(uint32_t target_id, aic_req_type_t req_type);

    uint32_t calc_issue_busy_cycles(uint32_t target_id, p_tm_pld_t pld) const;
    uint32_t calc_rsp_busy_cycles(uint32_t target_id, p_tm_pld_t pld) const;
    uint32_t target_outstanding(uint32_t target_id) const;

  private:
    uint32_t calc_payload_cycles(uint32_t width, uint32_t size) const;
    uint32_t calc_hotspot_penalty(uint32_t target_id) const;

  private:
    p_tm_bus_cfg_t cfg_ = nullptr;
    std::vector<uint32_t> rd_slot_credit_;
    std::vector<uint32_t> wr_slot_credit_;
    std::vector<uint32_t> acc_slot_credit_;
    std::vector<uint32_t> acc_bw_token_;
    std::vector<uint32_t> rd_bw_token_;
    std::vector<uint32_t> wr_bw_token_;
    std::vector<uint32_t> target_outstanding_;
};

#endif  // _TM_BUS_FLOW_CTRL_H_
