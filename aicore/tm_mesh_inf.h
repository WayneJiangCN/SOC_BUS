#ifndef _TM_MESH_INF_H_
#define _TM_MESH_INF_H_

#include <stdint.h>

#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "tm_clock.h"
#include "tm_engine.h"
#include "tm_inf.h"
#include "tm_mesh_topology.h"
#include "tm_mesh_types.h"
#include "tm_que.h"

using tm_mesh_grant_que_t = TmQue<TmMeshGrant>;
using p_tm_mesh_grant_que_t = std::shared_ptr<tm_mesh_grant_que_t>;

struct TmMeshInfApiReq
{
    aic_req_type_t req_type = aic_req_type_t::RD_REQ;
    uint32_t rsp_expected = 1;
    uint32_t rsp_seen = 0;
};

/*
 * Tm_mesh_inf
 *
 * master 侧的 endpoint / NIU。
 *
 * 这层主要负责：
 * 1. 从上游 bus_inf_ 收请求，放进本地 pending queue。
 * 2. 管理 API 风格请求的完成状态。
 * 3. 管理写事务的 grant，保证 WR_DAT 只能在拿到 grant 后前进。
 * 4. 将 mesh 返回的响应再发回上游 bus_inf_。
 *
 * 这里仍然是 message/transaction 级，不负责 router/link 的逐跳细节。
 */
class Tm_mesh_inf : public tm_engine::TmModule
{
  public:
    Tm_mesh_inf(const std::string& name, tm_engine::p_tm_clk_t clk,
                uint32_t inf_id, p_tm_mesh_cfg_t cfg);
    virtual ~Tm_mesh_inf();

    void config();
    void reset();
    bool idle();
    void tick();

    /* 将外部上游接口接到本 NIU 的 bus_inf_。 */
    void attach_upstream(p_tm_com_inf_t inf);
    /* 设置本 NIU 对应的 master_id。 */
    void set_master_id(uint32_t mst_id);

    /* API 风格请求入口：直接压入本地 pending queue。 */
    uint32_t send_rd_req(uint64_t address, uint32_t size);
    uint32_t send_wr_req(uint64_t address, uint32_t size);

    bool is_request_completed(uint32_t req_id);
    bool can_send_rd_req();
    bool can_send_wr_req();

    /*
     * 从 bus_inf_ 吸收上游请求。
     * 对 WR_REQ / RD_REQ 来说，如果 txn_ctx 里还没有建档，会顺便创建事务上下文。
     */
    void ingest_upstream_requests(
        uint32_t master_port, const TmMeshTopology& topology,
        std::unordered_map<uint64_t, TmMeshTxnCtx>& txn_ctx,
        tm_engine::tm_time_t now);

    /*
     * Fabric 通过这些接口从 NIU 读取待注入 mesh 的本地请求。
     * RD_REQ / WR_REQ 共用 req_pending_q_；
     * WR_DAT 单独走 wr_dat_pending_q_，因为它要受 grant 约束。
     */
    bool has_pending_request(aic_req_type_t req_type) const;
    p_tm_pld_t peek_pending_request(aic_req_type_t req_type) const;
    void pop_pending_request(aic_req_type_t req_type);

    /* 写事务 grant 的本地缓存。 */
    bool has_pending_grant() const;
    TmMeshGrant peek_pending_grant() const;
    void pop_pending_grant();
    bool can_accept_write_grant() const;

    /*
     * Fabric 将响应送回 source master 时，统一通过这三个入口进入 NIU。
     * 对 API 风格请求，会在这里做完成态退休；
     * 对普通上游请求，会把响应重新发到 bus_inf_。
     */
    bool accept_read_response(p_tm_pld_t rsp, uint32_t lane);
    bool accept_write_request_response(p_tm_pld_t rsp,
                                       const TmMeshGrant& grant);
    bool accept_write_data_response(p_tm_pld_t rsp);

  public:
    /* 对上游暴露的唯一接口：请求从这里进，响应从这里回。 */
    p_tm_com_inf_t bus_inf_ = nullptr;
    /* 本 NIU 对应的 master_id。 */
    uint32_t inf_id_ = 0;

  protected:
    tm_engine::p_tm_clk_t clk_ = nullptr;
    p_tm_mesh_cfg_t cfg_ = nullptr;

    /* 本地待发的 RD_REQ / WR_REQ，共用一个顺序队列。 */
    std::vector<p_tm_pld_t> req_pending_q_;
    /* 本地待发的 WR_DAT，单独建队以承接 grant 约束。 */
    std::vector<p_tm_pld_t> wr_dat_pending_q_;
    /* WR_REQ_RSP 带回来的 grant 缓存。 */
    p_tm_mesh_grant_que_t wr_grant_fifo_ = nullptr;

    /*
     * 仅用于 API 风格请求的本地完成跟踪。
     * bus_req_list_ 保存 req_id 和原始 pld 的对应关系；
     * api_req_map_ 保存每个 req_id 当前还缺多少响应。
     */
    std::vector<std::pair<uint32_t, p_tm_pld_t>> bus_req_list_;
    std::unordered_map<uint32_t, TmMeshInfApiReq> api_req_map_;

    uint32_t req_id_ = 0;

  protected:
    size_t request_queue_capacity() const;
    size_t write_data_queue_capacity() const;
    bool front_request_matches(aic_req_type_t req_type) const;

    uint32_t request_channel(aic_req_type_t req_type) const;
    uint32_t response_channel(aic_req_type_t req_type,
                              uint32_t lane = 0) const;

    uint64_t make_txn_key(uint32_t mst_id, uint32_t gid) const;
    uint64_t make_txn_key(p_tm_pld_t pld) const;

    void track_api_request(uint32_t req_id, p_tm_pld_t req,
                           aic_req_type_t req_type);
    void retire_tracked_request(p_tm_pld_t rsp);
    bool retire_api_read_response(p_tm_pld_t rsp);
    bool retire_api_write_response(p_tm_pld_t rsp);
    bool is_api_write_request(p_tm_pld_t rsp) const;
};

using tm_mesh_inf_t = Tm_mesh_inf;
using p_tm_mesh_inf_t = std::shared_ptr<tm_mesh_inf_t>;

inline p_tm_mesh_inf_t
tm_make_mesh_inf(const std::string& name, tm_engine::p_tm_clk_t clk,
                 uint32_t inf_id, p_tm_mesh_cfg_t cfg)
{
    return std::make_shared<Tm_mesh_inf>(name, clk, inf_id, cfg);
}

#endif  // _TM_MESH_INF_H_
