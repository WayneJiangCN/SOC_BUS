#ifndef _TM_RING_H_
#define _TM_RING_H_

#include <stdint.h>

#include <memory>
#include <unordered_map>
#include <vector>

#include "cfg.h"
#include "pem_log.h"
#include "tm_bus_flow_ctrl.h"
#include "tm_clock.h"
#include "tm_engine.h"
#include "tm_inf.h"
#include "tm_ring_topology.h"
#include "tm_ring_types.h"
#include "tm_que.h"

class PemBiu;
using p_tm_ring_biu_t = std::shared_ptr<PemBiu>;

class TmRingInf;
using tm_ring_inf_t = TmRingInf;
using p_tm_ring_inf_t = std::shared_ptr<tm_ring_inf_t>;

class TmRingRouter;
using tm_ring_router_t = TmRingRouter;
using p_tm_ring_router_t = std::shared_ptr<tm_ring_router_t>;

class TmRingLink;
using tm_ring_link_t = TmRingLink;
using p_tm_ring_link_t = std::shared_ptr<tm_ring_link_t>;

class TmRingTargetPort;
using tm_ring_target_port_t = TmRingTargetPort;
using p_tm_ring_target_port_t = std::shared_ptr<tm_ring_target_port_t>;

/*
 * Top-level ring interconnect model.
 *
 * Fabric owns the high-level topology objects: master NIUs, routers, links and
 * target ports. The fine-grained queue events stay inside each submodule; the
 * fabric callbacks only advance the affected link/router path.
 */
class TmRingFabric : public tm_engine::TmModule
{
  public:
    TmRingFabric();
    TmRingFabric(tm_engine::p_tm_clk_t clk, p_tm_ring_cfg_t cfg);
    virtual ~TmRingFabric();

    virtual void config();
    virtual void build();
    virtual void reset();
    virtual bool idle();

    virtual void attach_master(uint32_t idx, p_tm_ring_inf_t inf);
    virtual void attach_master(p_tm_ring_inf_t inf);
    virtual void attach_master(uint32_t idx, p_tm_ring_biu_t biu);
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
    p_tm_ring_cfg_t cfg_ = nullptr;

    std::vector<p_tm_ring_inf_t> master_nius_;
    std::vector<p_tm_ring_router_t> routers_;
    std::unordered_map<uint64_t, p_tm_ring_link_t> links_;
    std::vector<p_tm_ring_target_port_t> target_ports_;

    uint32_t ring_router_count_ = 0;
    uint32_t ring_link_latency_ = 1;

    std::shared_ptr<TmRingTopology> topology_;
    std::shared_ptr<TmBusFlowCtrl> flow_ctrl_;
    p_logger_t log_ = nullptr;

  protected:
    // Build stages used by config().
    void init_topology_and_flow_ctrl();
    void clear_components();
    void create_master_nius();
    void create_target_ports();
    void create_routers();
    void create_links();
    void create_link(uint32_t router_id, TmRingPortDir out_dir);

    // Wiring stages used by config() and attach APIs.
    void bind_master_nius();
    void attach_routers();
    void attach_links();
    void bind_target_ports();
    void bind_master_niu(uint32_t idx, p_tm_ring_inf_t inf);

    // Shared lookup helpers.
    p_tm_com_inf_t get_router_port_inf(uint32_t router_id,
                                        TmRingPortDir in_dir) const;

    p_tm_ring_link_t get_ring_link(uint32_t src_router_id,
                                   TmRingPortDir src_dir,
                                   uint32_t dst_router_id,
                                   TmRingPortDir dst_dir) const;
    uint64_t make_link_key(uint32_t src_router_id, TmRingPortDir src_dir,
                           uint32_t dst_router_id, TmRingPortDir dst_dir) const;

};

using tm_ring_fabric_t = TmRingFabric;
using p_tm_ring_fabric_t = std::shared_ptr<TmRingFabric>;

inline p_tm_ring_cfg_t
tm_make_ring_cfg()
{
    return std::make_shared<tm_ring_cfg_t>();
}

inline p_tm_ring_fabric_t
tm_make_ring(tm_engine::p_tm_clk_t clk, p_tm_ring_cfg_t cfg)
{
    return std::make_shared<TmRingFabric>(clk, cfg);
}

#endif  // _TM_RING_H_
