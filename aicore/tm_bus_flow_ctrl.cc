#include "tm_bus_flow_ctrl.h"

#include <algorithm>
#include <cassert>
#include <iostream>

using namespace std;
using namespace tm_engine;

/*
 * tm_bus_flow_ctrl.cc
 *
 * 这是当前 ring/bus 共用的 target 级事务流控实现。
 * 它不模拟 router/link credit，只模拟：
 * - target slot credit
 * - target bandwidth token
 * - target busy 周期
 * - outstanding 驱动的热点惩罚
 */

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

    /* 每个 target 独立维护自己的 slot/token/outstanding 状态。 */
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
    last_token_update_time_ = static_cast<tm_time_t>(-1);

    /* reset 时把所有 target 的资源恢复到满额状态。 */
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
    if (last_token_update_time_ == now) {
        return;
    }
    last_token_update_time_ = now;

    /* token 周期性恢复，近似表达 target 可持续提供的带宽。 */
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
    /*
     * 三类事务的约束不同：
     * - RD_REQ：既要读 slot，也要读带宽
     * - WR_REQ：只占写 slot，不占写数据带宽
     * - WR_DAT：主要受写带宽限制
     */
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



void
TmBusFlowCtrl::consume_target_credit(uint32_t target_id,
                                     aic_req_type_t req_type,
                                     p_tm_pld_t pld)
{
    /*
     * consume 发生在事务真正发给 target 之后。
     * WR_REQ 和 WR_DAT 分开扣，是为了保留两阶段写事务语义。
     */
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
    /* release 发生在响应真正完成后，而不是在请求发出时。 */
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

    if (target_outstanding_[target_id] == 0) {
        std::cerr << "TmBusFlowCtrl: target outstanding underflow on target "
                  << target_id << std::endl;
        assert(false && "target outstanding underflow");
        return;
    }
    target_outstanding_[target_id]--;
}

uint32_t
TmBusFlowCtrl::calc_issue_busy_cycles(uint32_t target_id, p_tm_pld_t pld) const
{
    auto target_cfg = cfg_->targets[target_id];

    /* 请求方向 busy 周期 = 固定前端/转发延迟 + payload 周期 + 热点惩罚。 */
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
    return calc_rsp_busy_cycles(target_id, pld, aic_req_type_t::RD_REQ);
}

uint32_t
TmBusFlowCtrl::calc_rsp_busy_cycles(uint32_t target_id, p_tm_pld_t pld,
                                    aic_req_type_t rsp_type) const
{
    auto target_cfg = cfg_->targets[target_id];
    uint32_t payload_size = calc_rsp_payload_size(pld, rsp_type);

    /* 响应方向和请求方向一样，只是使用 response_latency。 */
    return std::max<uint32_t>(
        1,
        target_cfg->response_latency + target_cfg->header_latency +
            calc_payload_cycles(target_cfg->width, payload_size) +
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
    /* 用 width 粗粒度折算 payload 需要多少拍传完。 */
    return std::max<uint32_t>(1, (size + width - 1) / width);
}

uint32_t
TmBusFlowCtrl::calc_rsp_payload_size(p_tm_pld_t pld,
                                     aic_req_type_t rsp_type) const
{
    if (pld == nullptr) {
        return 0;
    }
    if (rsp_type == aic_req_type_t::RD_REQ) {
        return pld->size;
    }
    return 0;
}

uint32_t
TmBusFlowCtrl::calc_hotspot_penalty(uint32_t target_id) const
{
    auto target_cfg = cfg_->targets[target_id];

    /* 当 outstanding 超阈值后，附加固定惩罚来放大热点效应。 */
    if (target_cfg->hotspot_penalty == 0) {
        return 0;
    }
    if (target_outstanding_[target_id] < target_cfg->hotspot_threshold) {
        return 0;
    }
    return target_cfg->hotspot_penalty;
}
