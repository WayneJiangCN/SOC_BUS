#include "tm_bus_flow_ctrl.h"

#include <algorithm>

using namespace std;
using namespace tm_engine;

namespace
{

inline uint32_t
clamp_add(uint32_t cur, uint32_t inc, uint32_t max_val)
{
    if (cur >= max_val) {
        return max_val;
    }
    uint64_t next = static_cast<uint64_t>(cur) + inc;
    return static_cast<uint32_t>(std::min<uint64_t>(next, max_val));
}

}  // namespace

void
TmBusFlowCtrl::config(p_tm_bus_cfg_t cfg)
{
    cfg_ = cfg;

    rd_slot_credit_.assign(cfg_->num_targets, 0);
    wr_slot_credit_.assign(cfg_->num_targets, 0);
    acc_slot_credit_.assign(cfg_->num_targets, 0);
    acc_bw_token_.assign(cfg_->num_targets, 0);
    rd_bw_token_.assign(cfg_->num_targets, 0);
    wr_bw_token_.assign(cfg_->num_targets, 0);
    target_outstanding_.assign(cfg_->num_targets, 0);

    for (uint32_t i = 0; i < cfg_->num_targets; ++i) {
    }
}

void
TmBusFlowCtrl::reset()
{

    for (uint32_t i = 0; i < cfg_->num_targets; ++i) {
        auto target_cfg = cfg_->targets[i];
        rd_slot_credit_[i] = target_cfg->rd_slot_credit_max;
        wr_slot_credit_[i] = target_cfg->wr_slot_credit_max;
        acc_slot_credit_[i] = target_cfg->acc_slot_credit_max;
        acc_bw_token_[i] = target_cfg->acc_bw_token_max;
        rd_bw_token_[i] = target_cfg->rd_bw_token_max;
        wr_bw_token_[i] = target_cfg->wr_bw_token_max;
        target_outstanding_[i] = 0;
    }
}

void
TmBusFlowCtrl::update_tokens(tm_time_t now)
{

    for (uint32_t i = 0; i < cfg_->num_targets; ++i) {
        auto target_cfg = cfg_->targets[i];

        if (target_cfg->token_update_period == 0) {
            continue;
        }
        if ((now % target_cfg->token_update_period) != 0) {
            continue;
        }

        acc_bw_token_[i] = clamp_add(acc_bw_token_[i],
                                     target_cfg->acc_bw_token_update,
                                     target_cfg->acc_bw_token_max);
        rd_bw_token_[i] = clamp_add(rd_bw_token_[i],
                                    target_cfg->rd_bw_token_update,
                                    target_cfg->rd_bw_token_max);
        wr_bw_token_[i] = clamp_add(wr_bw_token_[i],
                                    target_cfg->wr_bw_token_update,
                                    target_cfg->wr_bw_token_max);
    }
}

bool
TmBusFlowCtrl::can_send_to_target(uint32_t target_id, aic_req_type_t req_type,
                                  p_tm_pld_t pld) const
{

    auto size = pld->size;
    if (req_type == aic_req_type_t::RD_REQ) {
        return acc_slot_credit_[target_id] > 0 &&
               rd_slot_credit_[target_id] > 0 &&
               acc_bw_token_[target_id] >= size &&
               rd_bw_token_[target_id] >= size;
    }
    if (req_type == aic_req_type_t::WR_REQ) {
        return acc_slot_credit_[target_id] > 0 &&
               wr_slot_credit_[target_id] > 0;
    }

    return acc_bw_token_[target_id] >= size &&
           wr_bw_token_[target_id] >= size;
}

bool
TmBusFlowCtrl::wr_grant_match(uint32_t grant_target_id, uint32_t grant_gid,
                              uint32_t target_id, p_tm_pld_t pld,
                              bool strict_order) const
{

    if (strict_order && grant_gid != pld->gid) {
        return false;
    }
    return grant_target_id == target_id;
}

void
TmBusFlowCtrl::consume_target_credit(uint32_t target_id,
                                     aic_req_type_t req_type,
                                     p_tm_pld_t pld)
{

    auto size = pld->size;
    if (req_type == aic_req_type_t::RD_REQ) {
        acc_slot_credit_[target_id]--;
        rd_slot_credit_[target_id]--;
        acc_bw_token_[target_id] -= size;
        rd_bw_token_[target_id] -= size;
        target_outstanding_[target_id]++;
        return;
    }

    if (req_type == aic_req_type_t::WR_REQ) {
        acc_slot_credit_[target_id]--;
        wr_slot_credit_[target_id]--;
        target_outstanding_[target_id]++;
        return;
    }

    acc_bw_token_[target_id] -= size;
    wr_bw_token_[target_id] -= size;
}

void
TmBusFlowCtrl::release_target_credit(uint32_t target_id, aic_req_type_t req_type)
{

    auto target_cfg = cfg_->targets[target_id];

    acc_slot_credit_[target_id] =
        std::min(acc_slot_credit_[target_id] + 1,
                 target_cfg->acc_slot_credit_max);

    if (req_type == aic_req_type_t::RD_REQ) {
        rd_slot_credit_[target_id] =
            std::min(rd_slot_credit_[target_id] + 1,
                     target_cfg->rd_slot_credit_max);
    } else {
        wr_slot_credit_[target_id] =
            std::min(wr_slot_credit_[target_id] + 1,
                     target_cfg->wr_slot_credit_max);
    }

    target_outstanding_[target_id]--;
}

uint32_t
TmBusFlowCtrl::calc_issue_busy_cycles(uint32_t target_id, p_tm_pld_t pld) const
{
    auto target_cfg = cfg_->targets[target_id];

    return std::max<uint32_t>(
        1,
        target_cfg->frontend_latency + target_cfg->forward_latency +
            target_cfg->header_latency +
            calc_payload_cycles(target_cfg->width, pld->size) +
            calc_hotspot_penalty(target_id));
}

uint32_t
TmBusFlowCtrl::calc_rsp_busy_cycles(uint32_t target_id, p_tm_pld_t pld) const
{
    auto target_cfg = cfg_->targets[target_id];

    return std::max<uint32_t>(
        1,
        target_cfg->response_latency + target_cfg->header_latency +
            calc_payload_cycles(target_cfg->width, pld->size) +
            calc_hotspot_penalty(target_id));
}

uint32_t
TmBusFlowCtrl::target_outstanding(uint32_t target_id) const
{
    return target_outstanding_[target_id];
}

uint32_t
TmBusFlowCtrl::calc_payload_cycles(uint32_t width, uint32_t size) const
{
    return std::max<uint32_t>(1, (size + width - 1) / width);
}

uint32_t
TmBusFlowCtrl::calc_hotspot_penalty(uint32_t target_id) const
{
    auto target_cfg = cfg_->targets[target_id];

    if (target_cfg->hotspot_penalty == 0) {
        return 0;
    }
    if (target_outstanding_[target_id] < target_cfg->hotspot_threshold) {
        return 0;
    }
    return target_cfg->hotspot_penalty;
}
