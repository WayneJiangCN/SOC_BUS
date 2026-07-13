#ifndef _TM_MESH_TARGET_PORT_H_
#define _TM_MESH_TARGET_PORT_H_

#include <stdint.h>

#include <array>
#include <functional>
#include <memory>
#include <string>

#include "tm_clock.h"
#include "tm_engine.h"
#include "tm_inf.h"
#include "tm_mem.h"
#include "tm_mesh_types.h"
#include "tm_que.h"

/*
 * TmMeshTargetPort
 *
 * target 侧 endpoint / ingress port。
 *
 * 这层位于：
 *   Router -> TargetPort -> target/TmMem
 *
 * 它负责：
 * 1. 承接已经到达目标 router 的本地请求。
 * 2. 在 target 真正 ready 之前，先存在 target-local queue。
 * 3. 通过 inf_ 与下游 target / TmMem 做握手。
 */
class TmMeshTargetPort : public tm_engine::TmModule
{
  public:
    using event_callback_t = std::function<void()>;

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
    void attach(event_callback_t on_response, event_callback_t on_issue);
    void notify_issue_retry(aic_req_type_t req_type,
                            tm_engine::tm_time_t delay);

    /* 将 target 侧接口或 TmMem 接到本 TargetPort。 */
    void attach(p_tm_com_inf_t inf);
    void attach(p_tm_mem_t mem);

    /* 统一按请求类型访问 target-local queue。 */
    bool can_accept_request(aic_req_type_t req_type) const;
    void accept_request(aic_req_type_t req_type, p_tm_pld_t pld);
    bool has_request(aic_req_type_t req_type) const;
    p_tm_pld_t front_request(aic_req_type_t req_type) const;
    void pop_request(aic_req_type_t req_type);
    bool send_request(aic_req_type_t req_type, p_tm_pld_t pld);
    bool has_response(aic_req_type_t rsp_type, uint32_t lane = 0) const;
    p_tm_pld_t front_response(aic_req_type_t rsp_type,
                              uint32_t lane = 0) const;
    void pop_response(aic_req_type_t rsp_type, uint32_t lane = 0);

  private:
    void on_response_available();
    void on_request_available();
    void on_rd_issue_retry();
    void on_wr_req_issue_retry();
    void on_wr_dat_issue_retry();
    void notify_issue_callback();
    p_tm_com_que_t req_q(aic_req_type_t req_type) const;
    uint32_t response_channel(aic_req_type_t rsp_type,
                              uint32_t lane = 0) const;

    std::string name_;
    tm_engine::p_tm_clk_t clk_ = nullptr;
    p_tm_mesh_target_cfg_t cfg_ = nullptr;
    /* 对下游 target/TmMem 的握手接口。 */
    p_tm_com_inf_t inf_ = nullptr;

    /* target-local request queues。 */
    p_tm_com_que_t rd_req_q_ = nullptr;
    p_tm_com_que_t wr_req_q_ = nullptr;
    p_tm_com_que_t wr_dat_q_ = nullptr;
    std::array<tm_engine::p_tm_event_t, 3> issue_retry_events_{};
    event_callback_t on_response_;
    event_callback_t on_issue_;
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
