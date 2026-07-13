#ifndef _TM_MESH_TARGET_PORT_H_
#define _TM_MESH_TARGET_PORT_H_

#include <stdint.h>

#include <array>
#include <memory>
#include <string>
#include <vector>

#include "tm_clock.h"
#include "tm_engine.h"
#include "tm_inf.h"
#include "tm_mem.h"
#include "tm_mesh_types.h"
#include "tm_que.h"

class TmBusFlowCtrl;

/*
 * Target-side NIU.
 *
 * The ring router only ejects packets into these local queues. This module
 * owns the target/TmMem handshake: send requests to memory, receive memory
 * responses, and inject responses back into the local router FIFOs.
 */
class TmMeshTargetPort : public tm_engine::TmModule
{
  public:
    TmMeshTargetPort();
    TmMeshTargetPort(const std::string& name, tm_engine::p_tm_clk_t clk,
                     p_tm_mesh_target_cfg_t cfg, uint32_t rd_rsp_port_num,
                     uint32_t inf_depth);
    ~TmMeshTargetPort();

    void config(const std::string& name, tm_engine::p_tm_clk_t clk,
                p_tm_mesh_target_cfg_t cfg, uint32_t rd_rsp_port_num,
                uint32_t inf_depth);
    void reset();
    bool idle() const;

    void attach(uint32_t target_id, std::shared_ptr<TmBusFlowCtrl> flow_ctrl,
                const std::vector<p_tm_com_que_t>& rd_rsp_router_qs,
                p_tm_com_que_t wr_req_rsp_router_q,
                p_tm_com_que_t wr_dat_rsp_router_q);
    void attach(p_tm_com_inf_t inf);
    void attach(p_tm_mem_t mem);

    bool can_accept_request(aic_req_type_t req_type) const;
    void accept_request(aic_req_type_t req_type, p_tm_pld_t pld);
    bool has_request(aic_req_type_t req_type) const;
    p_tm_pld_t front_request(aic_req_type_t req_type) const;
    void pop_request(aic_req_type_t req_type);

    void send_pending_requests();
    void send_rd_cmd();
    void send_wr_cmd();
    void send_wr_dat();

    void recv_rd_cmd_rsp();
    void recv_wr_cmd_rsp();
    void recv_wr_dat_rsp();

    bool has_response(aic_req_type_t rsp_type, uint32_t lane = 0) const;
    p_tm_pld_t front_response(aic_req_type_t rsp_type,
                              uint32_t lane = 0) const;
    void pop_response(aic_req_type_t rsp_type, uint32_t lane = 0);

  private:
    void send_cmd(aic_req_type_t req_type);
    p_tm_com_que_t req_q(aic_req_type_t req_type) const;
    p_tm_com_que_t rsp_router_q(aic_req_type_t rsp_type,
                                uint32_t lane = 0) const;
    tm_engine::tm_time_t& next_req_issue_time(aic_req_type_t req_type);
    tm_engine::tm_time_t& next_rsp_issue_time(aic_req_type_t rsp_type,
                                              uint32_t lane = 0);
    uint32_t response_channel(aic_req_type_t rsp_type,
                              uint32_t lane = 0) const;

    std::string name_;
    tm_engine::p_tm_clk_t clk_ = nullptr;
    p_tm_mesh_target_cfg_t cfg_ = nullptr;
    uint32_t target_id_ = 0;
    uint32_t rd_rsp_port_num_ = 0;
    std::shared_ptr<TmBusFlowCtrl> flow_ctrl_ = nullptr;
    p_tm_com_inf_t inf_ = nullptr;

    p_tm_com_que_t rd_req_q_ = nullptr;
    p_tm_com_que_t wr_req_q_ = nullptr;
    p_tm_com_que_t wr_dat_q_ = nullptr;

    std::vector<p_tm_com_que_t> rd_rsp_router_qs_;
    p_tm_com_que_t wr_req_rsp_router_q_ = nullptr;
    p_tm_com_que_t wr_dat_rsp_router_q_ = nullptr;

    std::array<tm_engine::tm_time_t, 3> next_req_issue_time_ = {0, 0, 0};
    std::vector<tm_engine::tm_time_t> next_rd_rsp_issue_time_;
    tm_engine::tm_time_t next_wr_req_rsp_issue_time_ = 0;
    tm_engine::tm_time_t next_wr_dat_rsp_issue_time_ = 0;
};

using tm_mesh_target_port_t = TmMeshTargetPort;
using p_tm_mesh_target_port_t = std::shared_ptr<tm_mesh_target_port_t>;

inline p_tm_mesh_target_port_t
tm_make_mesh_target_port(const std::string& name, tm_engine::p_tm_clk_t clk,
                         p_tm_mesh_target_cfg_t cfg, uint32_t rd_rsp_port_num,
                         uint32_t inf_depth)
{
    return std::make_shared<TmMeshTargetPort>(name, clk, cfg, rd_rsp_port_num,
                                              inf_depth);
}

#endif  // _TM_MESH_TARGET_PORT_H_
