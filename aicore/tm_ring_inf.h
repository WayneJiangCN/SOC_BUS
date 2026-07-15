#ifndef _TM_RING_INF_H_
#define _TM_RING_INF_H_

#include <stdint.h>

#include <memory>
#include <string>
#include <unordered_map>
#include <utility>

#include "tm_clock.h"
#include "tm_engine.h"
#include "tm_inf.h"
#include "tm_ring_topology.h"
#include "tm_ring_types.h"
#include "tm_que.h"

using tm_ring_topology_t = TmRingTopology;
using p_tm_ring_topology_t = std::shared_ptr<tm_ring_topology_t>;

class TmBusFlowCtrl;

struct TmRingInfApiReq
{
    PldCmd cmd = PldCmd::RD;
    p_tm_pld_t req = nullptr;
    uint32_t rsp_expected = 1;
    uint32_t rsp_seen = 0;
};

/*
 * Master-side network interface unit.
 *
 * The NIU is the boundary between an upstream BIU/com_inf and the local source
 * router. It handles request metadata, write grant ordering and API-style
 * request completion tracking. Per-hop routing is still handled by routers.
 */
class TmRingInf : public tm_engine::TmModule
{
  public:
    TmRingInf(const std::string& name, tm_engine::p_tm_clk_t clk,
                uint32_t inf_id, p_tm_ring_cfg_t cfg);
    virtual ~TmRingInf();

    void config();
    void reset();
    bool idle();

    void attach(p_tm_com_inf_t inf);
    void attach(uint32_t master_port, p_tm_ring_topology_t topology,
                std::shared_ptr<TmBusFlowCtrl> flow_ctrl);
    void set_master_id(uint32_t mst_id);

    uint32_t send_rd_req(uint64_t address, uint32_t size);
    uint32_t send_wr_req(uint64_t address, uint32_t size);

    bool is_request_completed(uint32_t req_id);
    bool can_send_rd_req();
    bool can_send_wr_req();

    void recv_rd_cmd();
    void recv_wr_cmd();
    void recv_wr_dat();
    void send_rd_cmd();
    void send_wr_cmd();
    void send_wr_dat();
    void send_wr_dat_rsp();
    void recv_router_rsp();

    void release_read_osd();
    void release_write_osd();

  public:
    p_tm_com_inf_t bus_inf_ = nullptr;
    p_tm_com_inf_t router_inf() const;
    uint32_t inf_id_ = 0;

  protected:
    tm_engine::p_tm_clk_t clk_ = nullptr;
    p_tm_ring_cfg_t cfg_ = nullptr;

    uint32_t master_port_ = 0;
    p_tm_ring_topology_t topology_ = nullptr;
    std::shared_ptr<TmBusFlowCtrl> flow_ctrl_ = nullptr;
    p_tm_com_inf_t router_inf_ = nullptr;
    p_tm_com_que_t rd_cmds_ = nullptr;
    p_tm_com_que_t wr_cmds_ = nullptr;
    p_tm_com_que_t wr_data_ = nullptr;
    p_tm_com_que_t wr_dat_rsp_q_ = nullptr;

    std::unordered_map<uint32_t, TmRingInfApiReq> req_map_;
    std::unordered_map<uint64_t, p_tm_pld_t> pending_writes_;
    std::unordered_map<uint64_t, TmRingRdRspState> rd_rsp_states_;

    uint32_t req_id_ = 0;
    uint32_t rd_outstanding_ = 0;
    uint32_t wr_outstanding_ = 0;

  protected:
    uint32_t response_channel(PldCmd cmd, uint32_t lane = 0) const;

    void recv_cmd(PldCmd cmd);
    void send_cmd(PldCmd cmd);
    bool recv_rsp(p_tm_pld_t rsp);
    bool accept_rd_rsp(p_tm_pld_t rsp);
    bool accept_wr_req_rsp(p_tm_pld_t rsp);
    bool accept_wr_dat_rsp(p_tm_pld_t rsp);
    p_tm_com_que_t req_queue(PldCmd cmd) const;
    bool issue_cmd_to_ring(PldCmd cmd, p_tm_pld_t pld);
    void prepare_request_metadata(p_tm_pld_t pld, PldCmd cmd);
    bool can_reserve_master_osd(PldCmd cmd) const;
    void complete_rsp(p_tm_pld_t rsp);

    void track_request(uint32_t req_id, p_tm_pld_t req, PldCmd cmd);
    p_tm_pld_t make_write_data(p_tm_pld_t grant);
    bool retire_read_response(p_tm_pld_t rsp);
    bool retire_write_response(p_tm_pld_t rsp);
};

using tm_ring_inf_t = TmRingInf;
using p_tm_ring_inf_t = std::shared_ptr<tm_ring_inf_t>;

inline p_tm_ring_inf_t
tm_make_ring_inf(const std::string& name, tm_engine::p_tm_clk_t clk,
                 uint32_t inf_id, p_tm_ring_cfg_t cfg)
{
    return std::make_shared<TmRingInf>(name, clk, inf_id, cfg);
}

#endif  // _TM_RING_INF_H_
