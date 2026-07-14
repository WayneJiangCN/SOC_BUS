#ifndef _TM_RING_INF_H_
#define _TM_RING_INF_H_

#include <stdint.h>

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "tm_clock.h"
#include "tm_engine.h"
#include "tm_inf.h"
#include "tm_ring_topology.h"
#include "tm_ring_types.h"
#include "tm_que.h"

using tm_ring_grant_que_t = TmQue<p_tm_pld_t>;
using p_tm_ring_grant_que_t = std::shared_ptr<tm_ring_grant_que_t>;

class TmBusFlowCtrl;
using tm_ring_topology_t = TmRingTopology;
using p_tm_ring_topology_t = std::shared_ptr<tm_ring_topology_t>;
using tm_ring_flow_ctrl_t = TmBusFlowCtrl;
using p_tm_ring_flow_ctrl_t = std::shared_ptr<tm_ring_flow_ctrl_t>;
using tm_ring_osd_reserve_t = std::function<bool(aic_req_type_t)>;

struct TmRingInfApiReq
{
    aic_req_type_t req_type = aic_req_type_t::RD_REQ;
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
                p_tm_ring_flow_ctrl_t flow_ctrl, p_tm_com_que_t router_req_q,
                p_tm_com_que_t router_wr_dat_q,
                tm_ring_osd_reserve_t global_osd_reserve = nullptr);
    void set_master_id(uint32_t mst_id);

    uint32_t send_rd_req(uint64_t address, uint32_t size);
    uint32_t send_wr_req(uint64_t address, uint32_t size);

    bool is_request_completed(uint32_t req_id);
    bool can_send_rd_req();
    bool can_send_wr_req();

    void recv_rd_cmd();
    void recv_wr_cmd();
    void recv_wr_dat();

    void pop_pending_grant();
    void release_read_osd();
    void release_write_osd();

    bool accept_read_response(p_tm_pld_t rsp, uint32_t lane);
    bool accept_write_request_response(p_tm_pld_t rsp);
    bool accept_write_data_response(p_tm_pld_t rsp);

  public:
    p_tm_com_inf_t bus_inf_ = nullptr;
    uint32_t inf_id_ = 0;

  protected:
    tm_engine::p_tm_clk_t clk_ = nullptr;
    p_tm_ring_cfg_t cfg_ = nullptr;

    p_tm_ring_grant_que_t wr_grant_fifo_ = nullptr;

    uint32_t master_port_ = 0;
    p_tm_ring_topology_t topology_ = nullptr;
    p_tm_ring_flow_ctrl_t flow_ctrl_ = nullptr;
    p_tm_com_que_t router_req_q_ = nullptr;
    p_tm_com_que_t router_wr_dat_q_ = nullptr;
    tm_ring_osd_reserve_t global_osd_reserve_ = nullptr;

    std::vector<std::pair<uint32_t, p_tm_pld_t>> bus_req_list_;
    std::unordered_map<uint32_t, TmRingInfApiReq> api_req_map_;

    uint32_t req_id_ = 0;
    uint32_t rd_outstanding_ = 0;
    uint32_t wr_outstanding_ = 0;

  protected:
    uint32_t response_channel(aic_req_type_t req_type,
                              uint32_t lane = 0) const;

    bool issue_cmd_to_ring(aic_req_type_t req_type, p_tm_pld_t pld);
    void prepare_request_metadata(p_tm_pld_t pld, aic_req_type_t req_type);
    bool can_reserve_master_osd(aic_req_type_t req_type) const;
    bool reserve_transaction_osd(aic_req_type_t req_type);
    void track_api_request(uint32_t req_id, p_tm_pld_t req,
                           aic_req_type_t req_type);
    void retire_tracked_request(p_tm_pld_t rsp);
    bool retire_api_read_response(p_tm_pld_t rsp);
    bool retire_api_write_response(p_tm_pld_t rsp);
    bool is_api_write_request(p_tm_pld_t rsp) const;
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
