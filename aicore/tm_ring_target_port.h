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
 * Target 侧网络接口单元。
 *
 * Router 只负责把目的包弹出到 TargetPort。本模块负责 Target/TmMem 握手：
 * 向 Memory 发送请求、接收 Memory 响应，并将响应重新注入本地 Router。
 */
class TmRingTargetPort : public tm_engine::TmModule
{
  public:
    TmRingTargetPort();
    TmRingTargetPort(const std::string& name, tm_engine::p_tm_clk_t clk,
                     p_tm_ring_target_cfg_t cfg, uint32_t rd_rsp_port_num);
    ~TmRingTargetPort();

    void config(const std::string& name, tm_engine::p_tm_clk_t clk,
                p_tm_ring_target_cfg_t cfg, uint32_t rd_rsp_port_num);
    void reset();
    bool idle() const;

    // 绑定 Target 逻辑编号和共享流控，流控资源在真正发送到 Memory 时占用。
    void attach(uint32_t target_id, std::shared_ptr<TmBusFlowCtrl> flow_ctrl);
    // 绑定真实 Memory/TmMem 接口；inf_ 同时承载请求和存储返回的响应。
    void attach(p_tm_com_inf_t inf);
    void attach(p_tm_mem_t mem);
    p_tm_com_inf_t ring_inf() const;

    // 请求 FIFO 有效时分别尝试向 Memory 发送 RD、WR 和 WR_DAT。
    void send_pending_requests();
    void send_rd_cmd();
    void send_wr_cmd();
    void send_wr_dat();

    // Memory 响应到达后设置 Ring 响应类型，再注入本地 Router 的响应子网。
    void recv_rd_cmd_rsp();
    void recv_wr_cmd_rsp();
    void recv_wr_dat_rsp();
    void recv_ring_req();

    // 这些访问器读取 Memory 接口中的响应，不额外复制 payload。
    bool has_response(PldCmd cmd, uint32_t lane = 0) const;
    p_tm_pld_t front_response(PldCmd cmd,
                              uint32_t lane = 0) const;
    void pop_response(PldCmd cmd, uint32_t lane = 0);

  private:
    // 统一请求发送逻辑：检查 credit/token、发射间隔和下游 ready。
    void send_cmd(PldCmd cmd);
    // Ring 请求只有在对应 Target FIFO 未满时才 pop，形成自然反压。
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

    // Target 的真实请求缓存，分别保留读命令、写命令和写数据阶段。
    p_tm_com_que_t rd_req_q_ = nullptr;
    p_tm_com_que_t wr_req_q_ = nullptr;
    p_tm_com_que_t wr_dat_q_ = nullptr;

    // 发射时间用于限制 Target 前端吞吐，不是显式 retry 事件。
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
                         p_tm_ring_target_cfg_t cfg, uint32_t rd_rsp_port_num)
{
    return std::make_shared<TmRingTargetPort>(name, clk, cfg, rd_rsp_port_num);
}

#endif  // _TM_RING_TARGET_PORT_H_
