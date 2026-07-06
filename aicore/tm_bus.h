#ifndef _TM_BUS_H_
#define _TM_BUS_H_

#include <stdint.h>

#include <unordered_map>
#include <vector>

#include "cfg.h"
#include "pem_biu.h"
#include "tm_bus_arbiter.h"
#include "tm_bus_flow_ctrl.h"
#include "tm_bus_topology.h"
#include "tm_bus_types.h"
#include "tm_clock.h"
#include "tm_engine.h"
#include "tm_inf.h"
#include "tm_que.h"

class TmBusFabric : public tm_engine::TmModule
{
  public:
    TmBusFabric();
    TmBusFabric(tm_engine::p_tm_clk_t clk, p_tm_bus_cfg_t cfg);
    virtual ~TmBusFabric();

  public:
    virtual void config();
    virtual void build();
    virtual void reset();
    virtual bool idle();

    virtual void tick();

    virtual void attach_master(uint32_t idx, p_tm_com_inf_t inf);
    virtual void attach_master(uint32_t idx, p_pem_biu_t biu);
    virtual void attach_master(p_pem_biu_t biu);
    virtual void attach_target(uint32_t idx, p_tm_com_inf_t inf);
    virtual void attach_target(uint32_t idx, p_tm_mem_t mem);
    virtual void bind_master_id(uint32_t port_id, uint32_t mst_id);

  public:
    tm_engine::p_tm_clk_t clk_ = nullptr;
    p_tm_bus_cfg_t cfg_ = nullptr;

    std::vector<p_tm_com_inf_t> v_master_inf_;
    std::vector<p_tm_com_inf_t> v_target_inf_;

  protected:
    std::vector<p_tm_com_que_t> m_rd_req_fifo_;
    std::vector<p_tm_com_que_t> m_wr_req_fifo_;
    std::vector<p_tm_com_que_t> m_wr_dat_fifo_;

    std::vector<p_tm_com_que_t> t_rd_req_fifo_;
    std::vector<p_tm_com_que_t> t_wr_req_fifo_;
    std::vector<p_tm_com_que_t> t_wr_dat_fifo_;

    std::vector<std::vector<p_tm_com_que_t>> m_rd_rsp_fifo_;
    std::vector<p_tm_com_que_t> m_wr_req_rsp_fifo_;
    std::vector<p_tm_com_que_t> m_wr_dat_rsp_fifo_;

    std::vector<p_tm_com_que_t> n_rd_req_fifo_;
    std::vector<p_tm_com_que_t> n_wr_req_fifo_;
    std::vector<p_tm_com_que_t> n_wr_dat_fifo_;
    std::vector<std::vector<p_tm_com_que_t>> n_rd_rsp_fifo_;
    std::vector<p_tm_com_que_t> n_wr_req_rsp_fifo_;
    std::vector<p_tm_com_que_t> n_wr_dat_rsp_fifo_;

    std::vector<std::deque<TmBusGrant>> m_wr_grant_fifo_;
    std::unordered_map<uint64_t, TmBusTxnCtx> txn_ctx_;

    std::vector<tm_engine::tm_time_t> next_rd_issue_time_;
    std::vector<tm_engine::tm_time_t> next_wr_req_issue_time_;
    std::vector<tm_engine::tm_time_t> next_wr_dat_issue_time_;

    std::vector<std::vector<tm_engine::tm_time_t>> next_rd_rsp_issue_time_;
    std::vector<tm_engine::tm_time_t> next_wr_req_rsp_issue_time_;
    std::vector<tm_engine::tm_time_t> next_wr_dat_rsp_issue_time_;

    std::vector<tm_engine::tm_time_t> next_n_rd_req_hop_time_;
    std::vector<tm_engine::tm_time_t> next_n_wr_req_hop_time_;
    std::vector<tm_engine::tm_time_t> next_n_wr_dat_hop_time_;
    std::vector<tm_engine::tm_time_t> next_n_rd_rsp_hop_time_;
    std::vector<tm_engine::tm_time_t> next_n_wr_req_rsp_hop_time_;
    std::vector<tm_engine::tm_time_t> next_n_wr_dat_rsp_hop_time_;

    uint32_t ring_node_count_ = 0;
    uint32_t ring_link_latency_ = 1;

    TmBusTopology topology_;
    TmBusFlowCtrl flow_ctrl_;
    TmBusArbiter arbiter_;

  protected:
    void recv_master_reqs();
    void recv_master_req(uint32_t master_port, aic_req_type_t req_type);

    void inject_ring_reqs();
    void inject_ring_req(uint32_t master_port, aic_req_type_t req_type);
    void advance_ring_reqs();
    void advance_ring_req_type(aic_req_type_t req_type);

    void send_target_reqs();
    void send_target_req(uint32_t target_id, aic_req_type_t req_type);

    void recv_target_rsps();
    void recv_target_rd_rsp(uint32_t target_id, uint32_t lane);
    void recv_target_wr_req_rsp(uint32_t target_id);
    void recv_target_wr_dat_rsp(uint32_t target_id);

    void advance_ring_rsps();
    void advance_ring_rd_rsps();
    void advance_ring_wr_req_rsps();
    void advance_ring_wr_dat_rsps();

    void send_master_rsps();
    void send_master_rd_rsp(uint32_t master_port, uint32_t lane);
    void send_master_wr_req_rsp(uint32_t master_port);
    void send_master_wr_dat_rsp(uint32_t master_port);

    p_tm_com_que_t get_master_fifo(uint32_t master_port,
                                   aic_req_type_t req_type) const;
    p_tm_com_que_t get_target_fifo(uint32_t target_id,
                                   aic_req_type_t req_type) const;
    p_tm_com_que_t get_ring_req_fifo(uint32_t node_id,
                                     aic_req_type_t req_type) const;
    p_tm_com_que_t get_ring_rd_rsp_fifo(uint32_t node_id, uint32_t lane) const;
    p_tm_com_que_t get_ring_wr_req_rsp_fifo(uint32_t node_id) const;
    p_tm_com_que_t get_ring_wr_dat_rsp_fifo(uint32_t node_id) const;

    uint64_t make_txn_key(uint32_t mst_id, uint32_t gid) const;
    uint64_t make_txn_key(p_tm_pld_t pld) const;
};

using tm_bus_fabric_t = TmBusFabric;
using p_tm_bus_fabric_t = std::shared_ptr<TmBusFabric>;

inline p_tm_bus_cfg_t tm_make_bus_cfg()
{
    return std::make_shared<tm_bus_cfg_t>();
}

inline p_tm_bus_fabric_t tm_make_bus(tm_engine::p_tm_clk_t clk,
                                     p_tm_bus_cfg_t cfg)
{
    return std::make_shared<TmBusFabric>(clk, cfg);
}

#endif  // _TM_BUS_H_
