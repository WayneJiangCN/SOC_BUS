#ifndef _TM_RING_TARGET_PORT_H_
#define _TM_RING_TARGET_PORT_H_

#include <stdint.h>

#include <array>
#include <memory>
#include <string>
#include <vector>

#include "pem_log.h"
#include "tm_clock.h"
#include "tm_engine.h"
#include "tm_inf.h"
#include "tm_mem.h"
#include "tm_ring_types.h"
#include "tm_que.h"

class TmBusFlowCtrl;

/*
 * Target-side NIU.
 *
 * The ring router only ejects packets into these local queues. This module
 * owns the target/TmMem handshake: send requests to memory, receive memory
 * responses, and inject responses back into the local router FIFOs.
 */
class TmRingTargetPort : public tm_engine::TmModule
{
  public:
    TmRingTargetPort();
    TmRingTargetPort(const std::string& name, tm_engine::p_tm_clk_t clk,
                     p_tm_ring_target_cfg_t cfg, uint32_t rd_rsp_port_num,
                     uint32_t inf_delay);
    ~TmRingTargetPort();

    void config(const std::string& name, tm_engine::p_tm_clk_t clk,
                p_tm_ring_target_cfg_t cfg, uint32_t rd_rsp_port_num,
                uint32_t inf_delay);
    void reset();
    bool idle() const;

    void attach(uint32_t target_id, std::shared_ptr<TmBusFlowCtrl> flow_ctrl);
    void attach(p_tm_com_inf_t inf);
    void attach(p_tm_mem_t mem);
    p_tm_com_inf_t ring_inf() const;

    void send_pending_requests();
    void send_rd_cmd();
    void send_wr_cmd();
    void send_wr_dat();

    void recv_rd_cmd_rsp();
    void recv_wr_cmd_rsp();
    void recv_wr_dat_rsp();
    void recv_ring_req();

    bool has_response(PldCmd cmd, uint32_t lane = 0) const;
    p_tm_pld_t front_response(PldCmd cmd,
                              uint32_t lane = 0) const;
    void pop_response(PldCmd cmd, uint32_t lane = 0);

  private:
    void send_cmd(PldCmd cmd);
    void recv_ring_cmd(PldCmd cmd);
    p_tm_com_que_t req_q(PldCmd cmd) const;
    uint32_t ring_channel(PldCmd cmd, uint32_t lane = 0) const;
    tm_engine::tm_time_t& next_req_issue_time(PldCmd cmd);
    tm_engine::tm_time_t& next_rsp_issue_time(PldCmd cmd,
                                              uint32_t lane = 0);
    uint32_t response_channel(PldCmd cmd, uint32_t lane = 0) const;

    std::string name_;
    tm_engine::p_tm_clk_t clk_ = nullptr;
    p_tm_ring_target_cfg_t cfg_ = nullptr;
    uint32_t target_id_ = 0;
    uint32_t rd_rsp_port_num_ = 0;
    std::shared_ptr<TmBusFlowCtrl> flow_ctrl_ = nullptr;
    p_tm_com_inf_t inf_ = nullptr;
    p_tm_com_inf_t ring_inf_ = nullptr;

    p_tm_com_que_t rd_req_q_ = nullptr;
    p_tm_com_que_t wr_req_q_ = nullptr;
    p_tm_com_que_t wr_dat_q_ = nullptr;

    std::array<tm_engine::tm_time_t, 3> next_req_issue_time_ = {0, 0, 0};
    std::vector<tm_engine::tm_time_t> next_rd_rsp_issue_time_;
    tm_engine::tm_time_t next_wr_req_rsp_issue_time_ = 0;
    tm_engine::tm_time_t next_wr_dat_rsp_issue_time_ = 0;
    p_logger_t log_ = nullptr;
};

using tm_ring_target_port_t = TmRingTargetPort;
using p_tm_ring_target_port_t = std::shared_ptr<tm_ring_target_port_t>;

inline p_tm_ring_target_port_t
tm_make_ring_target_port(const std::string& name, tm_engine::p_tm_clk_t clk,
                         p_tm_ring_target_cfg_t cfg, uint32_t rd_rsp_port_num,
                         uint32_t inf_delay)
{
    return std::make_shared<TmRingTargetPort>(name, clk, cfg, rd_rsp_port_num,
                                              inf_delay);
}

#endif  // _TM_RING_TARGET_PORT_H_
