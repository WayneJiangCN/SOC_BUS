#ifndef _TM_BUS_ARBITER_H_
#define _TM_BUS_ARBITER_H_

#include <stdint.h>

#include <vector>

#include "tm_bus_types.h"

class TmBusArbiter
{
  public:
    void config(p_tm_bus_cfg_t cfg);
    void reset();

    bool pick_master(aic_req_type_t req_type, uint32_t target_id,
                     const std::vector<uint8_t>& eligible_mask,
                     uint32_t& winner);

  private:
    std::vector<uint32_t>* rr_state(aic_req_type_t req_type);
    bool pick_rr(aic_req_type_t req_type, uint32_t target_id,
                 const std::vector<uint8_t>& eligible_mask,
                 uint32_t& winner);
    bool pick_islip_like(aic_req_type_t req_type, uint32_t target_id,
                         const std::vector<uint8_t>& eligible_mask,
                         uint32_t& winner);

  private:
    p_tm_bus_cfg_t cfg_ = nullptr;
    std::vector<uint32_t> rr_rd_ptr_;
    std::vector<uint32_t> rr_wr_req_ptr_;
    std::vector<uint32_t> rr_wr_dat_ptr_;
};

#endif  // _TM_BUS_ARBITER_H_
