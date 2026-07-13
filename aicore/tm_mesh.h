#ifndef _TM_MESH_H_
#define _TM_MESH_H_

#include <stdint.h>

#include <array>
#include <memory>
#include <unordered_map>
#include <vector>

#include "cfg.h"
#include "tm_bus_flow_ctrl.h"
#include "tm_clock.h"
#include "tm_engine.h"
#include "tm_inf.h"
#include "tm_mesh_topology.h"
#include "tm_mesh_types.h"
#include "tm_que.h"

class PemBiu;
using p_tm_mesh_biu_t = std::shared_ptr<PemBiu>;

class Tm_mesh_inf;
using tm_mesh_inf_t = Tm_mesh_inf;
using p_tm_mesh_inf_t = std::shared_ptr<tm_mesh_inf_t>;

class TmMeshRouter;
using tm_mesh_router_t = TmMeshRouter;
using p_tm_mesh_router_t = std::shared_ptr<tm_mesh_router_t>;

class TmMeshLink;
using tm_mesh_link_t = TmMeshLink;
using p_tm_mesh_link_t = std::shared_ptr<tm_mesh_link_t>;

class TmMeshTargetPort;
using tm_mesh_target_port_t = TmMeshTargetPort;
using p_tm_mesh_target_port_t = std::shared_ptr<tm_mesh_target_port_t>;

/*
 * Top-level ring interconnect model.
 *
 * Fabric owns the high-level topology objects: master NIUs, routers, links and
 * target ports. The fine-grained queue events stay inside each submodule; the
 * fabric callbacks only advance the affected link/router path.
 */
class TmMeshFabric : public tm_engine::TmModule
{
  public:
    TmMeshFabric();
    TmMeshFabric(tm_engine::p_tm_clk_t clk, p_tm_mesh_cfg_t cfg);
    virtual ~TmMeshFabric();

    virtual void config();
    virtual void build();
    virtual void reset();
    virtual bool idle();

    virtual void update_target_tokens();

    virtual void attach_master(uint32_t idx, p_tm_mesh_inf_t inf);
    virtual void attach_master(p_tm_mesh_inf_t inf);
    virtual void attach_master(uint32_t idx, p_tm_mesh_biu_t biu);
    virtual void attach_master(uint32_t idx, p_tm_com_inf_t inf);
    virtual void attach_target(uint32_t idx, p_tm_com_inf_t inf);
    virtual void attach_target(uint32_t idx, p_tm_mem_t mem);
    virtual void bind_master_id(uint32_t port_id, uint32_t mst_id);

    virtual uint32_t send_rd_req(uint32_t master_port, uint64_t address,
                                 uint32_t size);
    virtual uint32_t send_wr_req(uint32_t master_port, uint64_t address,
                                 uint32_t size);
    virtual bool completed(uint32_t master_port, uint32_t req_id);
    virtual bool canSendRdReq(uint32_t master_port);
    virtual bool canSendWrReq(uint32_t master_port);

  protected:
    tm_engine::p_tm_clk_t clk_ = nullptr;
    p_tm_mesh_cfg_t cfg_ = nullptr;

    std::vector<p_tm_mesh_inf_t> master_nius_;
    std::vector<p_tm_mesh_router_t> routers_;
    std::unordered_map<uint64_t, p_tm_mesh_link_t> links_;
    std::vector<p_tm_mesh_target_port_t> target_ports_;

    std::unordered_map<uint64_t, TmMeshRdRspState> rd_rsp_states_;

    std::vector<tm_engine::tm_time_t> next_rd_issue_time_;
    std::vector<tm_engine::tm_time_t> next_wr_req_issue_time_;
    std::vector<tm_engine::tm_time_t> next_wr_dat_issue_time_;
    tm_engine::p_tm_clk_t token_clk_ = nullptr;

    uint32_t ring_router_count_ = 0;
    uint32_t ring_link_latency_ = 1;

    std::shared_ptr<TmMeshTopology> topology_;
    std::shared_ptr<TmBusFlowCtrl> flow_ctrl_;

  protected:
    bool resolve_candidate_route(uint32_t router_id,
                                 TmMeshRouteCandidate& cand);
    bool check_candidate_ready(uint32_t router_id,
                               const TmMeshRouteCandidate& cand);
    bool commit_router_candidate(uint32_t router_id,
                                 const TmMeshRouteCandidate& cand);
    bool commit_local_router_candidate(uint32_t router_id,
                                       const TmMeshRouteCandidate& cand);
    bool commit_link_router_candidate(uint32_t router_id,
                                      const TmMeshRouteCandidate& cand);
    TmRingSubnet ring_subnet(uint32_t traffic_class) const;

    void send_target_reqs();
    void send_target_req(uint32_t target_id, aic_req_type_t req_type);
    void schedule_target_issue_retry(uint32_t target_id, aic_req_type_t req_type,
                                     tm_engine::tm_time_t delay);

    void recv_target_rsps();
    void recv_target_rd_rsp(uint32_t target_id, uint32_t lane);
    void recv_target_wr_req_rsp(uint32_t target_id);
    void recv_target_wr_dat_rsp(uint32_t target_id);

    p_tm_com_que_t get_router_req_fifo(uint32_t router_id,
                                       TmMeshPortDir in_dir,
                                       aic_req_type_t req_type) const;
    p_tm_com_que_t get_router_rd_rsp_fifo(uint32_t router_id,
                                          TmMeshPortDir in_dir,
                                          uint32_t lane) const;
    p_tm_com_que_t get_router_wr_req_rsp_fifo(uint32_t router_id,
                                              TmMeshPortDir in_dir) const;
    p_tm_com_que_t get_router_wr_dat_rsp_fifo(uint32_t router_id,
                                              TmMeshPortDir in_dir) const;

    p_tm_mesh_link_t get_ring_link(uint32_t src_router_id,
                                   TmMeshPortDir src_dir,
                                   uint32_t dst_router_id,
                                   TmMeshPortDir dst_dir) const;
    uint64_t make_link_key(uint32_t src_router_id, TmMeshPortDir src_dir,
                           uint32_t dst_router_id, TmMeshPortDir dst_dir) const;

    uint64_t make_txn_key(uint32_t mst_id, uint32_t gid) const;
    uint64_t make_txn_key(p_tm_pld_t pld) const;
};

using tm_mesh_fabric_t = TmMeshFabric;
using p_tm_mesh_fabric_t = std::shared_ptr<TmMeshFabric>;

inline p_tm_mesh_cfg_t
tm_make_mesh_cfg()
{
    return std::make_shared<tm_mesh_cfg_t>();
}

inline p_tm_mesh_fabric_t
tm_make_mesh(tm_engine::p_tm_clk_t clk, p_tm_mesh_cfg_t cfg)
{
    return std::make_shared<TmMeshFabric>(clk, cfg);
}

#endif  // _TM_MESH_H_
