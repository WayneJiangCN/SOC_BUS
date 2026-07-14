#ifndef _TM_RING_ROUTER_H_
#define _TM_RING_ROUTER_H_

#include <stdint.h>

#include <memory>
#include <string>
#include <vector>

#include "tm_clock.h"
#include "tm_engine.h"
#include "tm_ring_link.h"
#include "tm_ring_topology.h"
#include "tm_ring_types.h"
#include "tm_que.h"

class TmRingFabric;
class TmRingInf;
class TmRingTargetPort;
class TmRingRouter : public tm_engine::TmModule
{
  public:
    TmRingRouter();
    TmRingRouter(const std::string& name, tm_engine::p_tm_clk_t clk,
                 p_tm_ring_cfg_t cfg);
    ~TmRingRouter();

    void config(const std::string& name, tm_engine::p_tm_clk_t clk,
                p_tm_ring_cfg_t cfg);
    void reset();
    bool idle() const;
    void attach(uint32_t router_id,
                std::shared_ptr<TmRingTopology> topology,
                p_tm_ring_link_t east_link, p_tm_ring_link_t west_link,
                std::vector<std::shared_ptr<TmRingInf>>* master_nius,
                std::vector<std::shared_ptr<TmRingTargetPort>>* target_ports,
                TmRingFabric* fabric);

    p_tm_com_inf_t req_inf(TmRingPortDir in_dir) const;
    p_tm_com_inf_t wr_dat_inf(TmRingPortDir in_dir) const;
    p_tm_com_inf_t rd_rsp_inf(TmRingPortDir in_dir, uint32_t lane) const;
    p_tm_com_inf_t wr_req_rsp_inf(TmRingPortDir in_dir) const;
    p_tm_com_inf_t wr_dat_rsp_inf(TmRingPortDir in_dir) const;

  private:
    void route_request();
    void route_response();
    void advance_subnet(TmRingSubnet subnet);
    p_tm_pld_t select_output_candidate(TmRingPortDir out_dir,
                                       TmRingSubnet subnet);
    void resolve_route(p_tm_pld_t pld);
    bool route_ready(p_tm_pld_t pld);
    bool route_packet(p_tm_pld_t pld);
    bool local_ready(p_tm_pld_t pld);
    bool route_local(p_tm_pld_t pld);
    p_tm_ring_link_t output_link(TmRingPortDir out_dir) const;

    uint32_t traffic_slot_count() const;
    void decode_slot(uint32_t slot_class, uint32_t& traffic_class,
                     uint32_t& rsp_lane);
    p_tm_com_inf_t inf_for_class(TmRingPortDir in_dir,
                                   uint32_t traffic_class,
                                   uint32_t lane = 0) const;
    std::shared_ptr<TmRingInf> local_master(p_tm_pld_t pld) const;
    std::shared_ptr<TmRingTargetPort> local_target(p_tm_pld_t pld) const;

    std::string name_;
    tm_engine::p_tm_clk_t clk_ = nullptr;
    p_tm_ring_cfg_t cfg_ = nullptr;

    std::vector<p_tm_com_inf_t> req_infs_;
    std::vector<p_tm_com_inf_t> wr_dat_infs_;
    std::vector<std::vector<p_tm_com_inf_t>> rd_rsp_infs_;
    std::vector<p_tm_com_inf_t> wr_req_rsp_infs_;
    std::vector<p_tm_com_inf_t> wr_dat_rsp_infs_;

    std::vector<uint32_t> output_rr_ptr_;
    uint32_t router_id_ = 0;
    std::shared_ptr<TmRingTopology> topology_ = nullptr;
    p_tm_ring_link_t east_link_ = nullptr;
    p_tm_ring_link_t west_link_ = nullptr;
    std::vector<std::shared_ptr<TmRingInf>>* master_nius_ = nullptr;
    std::vector<std::shared_ptr<TmRingTargetPort>>* target_ports_ = nullptr;
    TmRingFabric* fabric_ = nullptr;
};

using tm_ring_router_t = TmRingRouter;
using p_tm_ring_router_t = std::shared_ptr<tm_ring_router_t>;

inline p_tm_ring_router_t
tm_make_ring_router(const std::string& name, tm_engine::p_tm_clk_t clk,
                    p_tm_ring_cfg_t cfg)
{
    return std::make_shared<TmRingRouter>(name, clk, cfg);
}

#endif  // _TM_RING_ROUTER_H_
