#ifndef _TM_MESH_ROUTER_H_
#define _TM_MESH_ROUTER_H_

#include <stdint.h>

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "tm_clock.h"
#include "tm_engine.h"
#include "tm_mesh_types.h"
#include "tm_que.h"

/*
 * TmMeshRouter
 *
 * A coarse transaction-level router aligned with the SimpleNetwork idea:
 * - the router owns local input message buffers
 * - the fabric asks the router to pick one winner for each output
 * - link timing is modeled separately by TmMeshLink
 *
 * This router does not model flits, VCs, or a crossbar. Instead it behaves
 * like a message-level switch:
 * - each traffic class contributes its queue head as a candidate
 * - candidates that want the same output port compete locally
 * - the router chooses a winner with per-output round-robin state
 */
class TmMeshRouter
{
  public:
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

    p_tm_com_que_t req_q() const;
    p_tm_com_que_t wr_dat_q() const;
    p_tm_com_que_t rd_rsp_q(uint32_t lane) const;
    p_tm_com_que_t wr_req_rsp_q() const;
    p_tm_com_que_t wr_dat_rsp_q() const;

    uint32_t traffic_class_count() const;
    p_tm_com_que_t queue_for_class(uint32_t traffic_class) const;
    p_tm_pld_t front_packet(uint32_t traffic_class) const;
    void pop_packet(uint32_t traffic_class);

    bool pick_output_winner(uint64_t output_key,
                            const std::vector<uint8_t>& eligible_mask,
                            uint32_t& winner);

  private:
    std::string name_;
    tm_engine::p_tm_clk_t clk_ = nullptr;
    p_tm_mesh_cfg_t cfg_ = nullptr;

    p_tm_com_que_t req_q_ = nullptr;
    p_tm_com_que_t wr_dat_q_ = nullptr;
    std::vector<p_tm_com_que_t> rd_rsp_qs_;
    p_tm_com_que_t wr_req_rsp_q_ = nullptr;
    p_tm_com_que_t wr_dat_rsp_q_ = nullptr;

    std::unordered_map<uint64_t, uint32_t> output_rr_ptr_;
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
