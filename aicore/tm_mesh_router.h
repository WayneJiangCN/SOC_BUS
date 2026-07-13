#ifndef _TM_MESH_ROUTER_H_
#define _TM_MESH_ROUTER_H_

#include <stdint.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "tm_clock.h"
#include "tm_engine.h"
#include "tm_mesh_types.h"
#include "tm_que.h"

class TmMeshRouter : public tm_engine::TmModule
{
  public:
    using RouteCandidate = TmMeshRouteCandidate;
    using route_resolver_t =
        std::function<bool(uint32_t router_id, RouteCandidate& cand)>;
    using route_ready_t =
        std::function<bool(uint32_t router_id, const RouteCandidate& cand)>;
    using route_commit_t =
        std::function<bool(uint32_t router_id, const RouteCandidate& cand)>;

    enum : uint32_t
    {
        REQ_CLASS = 0,
        WR_DAT_CLASS = 1,
        WR_REQ_RSP_CLASS = 2,
        WR_DAT_RSP_CLASS = 3,
        RD_RSP_BASE_CLASS = 4,
    };

    TmMeshRouter();
    TmMeshRouter(const std::string& name, tm_engine::p_tm_clk_t clk,
                 p_tm_mesh_cfg_t cfg);
    ~TmMeshRouter();

    void config(const std::string& name, tm_engine::p_tm_clk_t clk,
                p_tm_mesh_cfg_t cfg);
    void reset();
    bool idle() const;
    void attach(uint32_t router_id, route_resolver_t route_resolver,
                route_ready_t route_ready, route_commit_t route_commit);

    p_tm_com_que_t req_q(TmMeshPortDir in_dir) const;
    p_tm_com_que_t wr_dat_q(TmMeshPortDir in_dir) const;
    p_tm_com_que_t rd_rsp_q(TmMeshPortDir in_dir, uint32_t lane) const;
    p_tm_com_que_t wr_req_rsp_q(TmMeshPortDir in_dir) const;
    p_tm_com_que_t wr_dat_rsp_q(TmMeshPortDir in_dir) const;

  private:
    enum class TrafficKind : uint32_t
    {
        REQ = 0,
        WR_DAT = 1,
        RSP = 2,
    };

    void route_request();
    void route_write_data();
    void route_response();
    void advance_traffic(TrafficKind traffic_kind);
    bool select_output_candidate(TmMeshPortDir out_dir,
                                 TrafficKind traffic_kind,
                                 RouteCandidate& winner);

    uint32_t traffic_class_count() const;
    TrafficKind traffic_kind(uint32_t traffic_class) const;
    p_tm_com_que_t queue_for_class(TmMeshPortDir in_dir,
                                   uint32_t traffic_class) const;
    p_tm_pld_t front_packet(TmMeshPortDir in_dir, uint32_t traffic_class) const;
    void pop_packet(TmMeshPortDir in_dir, uint32_t traffic_class);
    bool pick_output_winner(TmMeshPortDir out_dir, TrafficKind traffic_kind,
                            uint32_t candidate_count, uint32_t& winner);

    std::string name_;
    tm_engine::p_tm_clk_t clk_ = nullptr;
    p_tm_mesh_cfg_t cfg_ = nullptr;

    std::vector<p_tm_com_que_t> req_qs_;
    std::vector<p_tm_com_que_t> wr_dat_qs_;
    std::vector<std::vector<p_tm_com_que_t>> rd_rsp_qs_;
    std::vector<p_tm_com_que_t> wr_req_rsp_qs_;
    std::vector<p_tm_com_que_t> wr_dat_rsp_qs_;

    std::vector<uint32_t> output_rr_ptr_;
    uint32_t router_id_ = 0;
    route_resolver_t route_resolver_;
    route_ready_t route_ready_;
    route_commit_t route_commit_;
};

using tm_mesh_router_t = TmMeshRouter;
using p_tm_mesh_router_t = std::shared_ptr<tm_mesh_router_t>;

inline p_tm_mesh_router_t
tm_make_mesh_router(const std::string& name, tm_engine::p_tm_clk_t clk,
                    p_tm_mesh_cfg_t cfg)
{
    return std::make_shared<TmMeshRouter>(name, clk, cfg);
}

#endif  // _TM_MESH_ROUTER_H_
