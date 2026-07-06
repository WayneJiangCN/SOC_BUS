#include "tm_bus_arbiter.h"

#include <algorithm>

using namespace std;

void
TmBusArbiter::config(p_tm_bus_cfg_t cfg)
{
    cfg_ = cfg;

    rr_rd_ptr_.assign(cfg_->num_targets, 0);
    rr_wr_req_ptr_.assign(cfg_->num_targets, 0);
    rr_wr_dat_ptr_.assign(cfg_->num_targets, 0);
}

void
TmBusArbiter::reset()
{
    std::fill(rr_rd_ptr_.begin(), rr_rd_ptr_.end(), 0);
    std::fill(rr_wr_req_ptr_.begin(), rr_wr_req_ptr_.end(), 0);
    std::fill(rr_wr_dat_ptr_.begin(), rr_wr_dat_ptr_.end(), 0);
}

bool
TmBusArbiter::pick_master(aic_req_type_t req_type, uint32_t target_id,
                          const std::vector<uint8_t>& eligible_mask,
                          uint32_t& winner)
{

    winner = 0;
    if (cfg_->arbiter_type == tm_bus_arbiter_type_t::ISLIP_LIKE) {
        return pick_islip_like(req_type, target_id, eligible_mask, winner);
    }
    return pick_rr(req_type, target_id, eligible_mask, winner);
}

std::vector<uint32_t>*
TmBusArbiter::rr_state(aic_req_type_t req_type)
{
    if (req_type == aic_req_type_t::RD_REQ) {
        return &rr_rd_ptr_;
    }
    if (req_type == aic_req_type_t::WR_REQ) {
        return &rr_wr_req_ptr_;
    }
    return &rr_wr_dat_ptr_;
}

bool
TmBusArbiter::pick_rr(aic_req_type_t req_type, uint32_t target_id,
                      const std::vector<uint8_t>& eligible_mask,
                      uint32_t& winner)
{
    auto* state = rr_state(req_type);

    uint32_t start = (*state)[target_id];
    for (uint32_t step = 0; step < cfg_->num_masters; ++step) {
        uint32_t master = (start + step) % cfg_->num_masters;
        if (eligible_mask[master] == 0) {
            continue;
        }

        winner = master;
        (*state)[target_id] = (master + 1) % cfg_->num_masters;
        return true;
    }
    return false;
}

bool
TmBusArbiter::pick_islip_like(aic_req_type_t req_type, uint32_t target_id,
                              const std::vector<uint8_t>& eligible_mask,
                              uint32_t& winner)
{
    return pick_rr(req_type, target_id, eligible_mask, winner);
}
